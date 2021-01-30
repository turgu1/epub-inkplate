#define __PAGE_LOCS__ 1
#include "models/page_locs.hpp"
#include "models/config.hpp"
#include "controllers/event_mgr.hpp"

#include "viewers/book_viewer.hpp"
#include "viewers/page.hpp"
#include "logging.hpp"

#include "stb_image.h"

#include <iostream>
#include <fstream>
#include <ios>

enum class MgrReq : int8_t { ASAP_READY, STOPPED };

struct MgrQueueData {
  MgrReq req;
  int16_t itemref_index;
};

enum class StateReq  : int8_t { ABORT, STOP, START_DOCUMENT, GET_ASAP, ITEM_READY, ASAP_READY };

struct StateQueueData {
  StateReq req;
  int16_t itemref_index;
  int16_t itemref_count;
};

enum class RetrieveReq  : int8_t { ABORT, RETRIEVE_ITEM, GET_ASAP };

struct RetrieveQueueData {
  RetrieveReq req;
  int16_t itemref_index;
};

#if EPUB_LINUX_BUILD
  #include <chrono>

  static mqd_t mgr_queue;
  static mqd_t state_queue;
  static mqd_t retrieve_queue;

  static mq_attr mgr_attr      = { 0, 5, sizeof(     MgrQueueData), 0 };
  static mq_attr state_attr    = { 0, 5, sizeof(   StateQueueData), 0 };
  static mq_attr retrieve_attr = { 0, 5, sizeof(RetrieveQueueData), 0 };

  #define QUEUE_SEND(q, m, t)        mq_send(q, (const char *) &m, sizeof(m),       1)
  #define QUEUE_RECEIVE(q, m, t)  mq_receive(q,       (char *) &m, sizeof(m), nullptr)
#else
  #include <esp_pthread.h>

  static esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio)
  {
      auto cfg = esp_pthread_get_default_config();
      cfg.thread_name = name;
      cfg.pin_to_core = core_id;
      cfg.stack_size = stack;
      cfg.prio = prio;
      return cfg;
  }

  static xQueueHandle mgr_queue;
  static xQueueHandle state_queue;
  static xQueueHandle retrieve_queue;

  #define QUEUE_SEND(q, m, t)        xQueueSend(q, &m, t)
  #define QUEUE_RECEIVE(q, m, t)  xQueueReceive(q, &m, t)
#endif

class StateTask
{
  private:
    static constexpr const char * TAG = "StateTask";

    bool retriever_iddle;

    int16_t   itemref_count;       // Number of items in the document
    int16_t   waiting_for_itemref; // Current item being processed by retrieval task
    int16_t   next_itemref_to_get; // Non prioritize item to get next
    int16_t   asap_itemref;        // Prioritize item to get next
    uint8_t * bitset;              // Set of all items processed so far
    uint8_t   bitset_size;         // bitset byte length
    bool      stopping;
    bool      forget_retrieval;    // Forget current item begin processed by retrieval task

    StateQueueData       state_queue_data;
    RetrieveQueueData retrieve_queue_data;
    MgrQueueData           mgr_queue_data;

    /**
     * @brief Request next item to be retrieved
     *
     * This function is called to identify and send the
     * next request for retrieval of pages location. It also
     * identify when the whole process is completed, as all items from
     * the document have been done. It will then send this information
     * to the appliction through the Mgr queue.
     *
     * When this function is called, the retrieval task is waiting for
     * the next task to do.
     *
     * @param itemref The last itemref index that was processed
     */
    void request_next_item(int16_t itemref,
                          bool already_sent_to_mgr = false)
    {
      if (asap_itemref != -1) {
        if (itemref == asap_itemref) {
          asap_itemref = -1;
          if (!already_sent_to_mgr) {
            mgr_queue_data.itemref_index = itemref;
            mgr_queue_data.req           = MgrReq::ASAP_READY;
            QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
            LOG_D("Sent ASAP_READY to Mgr");
          }
        } else {
          waiting_for_itemref               = asap_itemref;
          asap_itemref                      = -1;
          retrieve_queue_data.req           = RetrieveReq::GET_ASAP;
          retrieve_queue_data.itemref_index = waiting_for_itemref;
          QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
          retriever_iddle                   = false;
          LOG_D("Sent GET_ASAP to Retriever");
          return;
        }
      }
      if (next_itemref_to_get != -1) {
        waiting_for_itemref               = next_itemref_to_get;
        next_itemref_to_get               = -1;
        retrieve_queue_data.req           = RetrieveReq::RETRIEVE_ITEM;
        retrieve_queue_data.itemref_index = waiting_for_itemref;
        QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
        retriever_iddle                   = false;
        LOG_D("Sent RETRIEVE_ITEM to Retriever");
      } else {
        int16_t newref;
        if (itemref != -1) {
          newref = (itemref + 1) % itemref_count;
        } else {
          itemref = 0;
          newref = 0;
        }
        while ((bitset[newref >> 3] & (1 << (newref & 7))) != 0) {
          newref = (newref + 1) % itemref_count;
          if (newref == itemref)
            break;
        }
        if (newref != itemref) {
          waiting_for_itemref               = newref;
          retrieve_queue_data.req           = RetrieveReq::RETRIEVE_ITEM;
          retrieve_queue_data.itemref_index = waiting_for_itemref;
          QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
          retriever_iddle                   = false;
          LOG_D("Sent RETRIEVE_ITEM to Retriever");
        } else {
          page_locs.computation_completed();
          retriever_iddle = true;
        }
      }
    }

  public:
    StateTask() : 
          retriever_iddle(   true), 
            itemref_count(     -1),
      waiting_for_itemref(     -1),
      next_itemref_to_get(     -1),
             asap_itemref(     -1),
                   bitset(nullptr),
              bitset_size(      0),
                 stopping(  false),
         forget_retrieval(  false)  { }

    void operator()() {
      for(;;) {
        LOG_D("==> Waiting for request... <==");
        if (QUEUE_RECEIVE(state_queue, state_queue_data, portMAX_DELAY) == -1) {
          LOG_E("Receive error: %d: %s", errno, strerror(errno));
        }
        else switch (state_queue_data.req) {
          case StateReq::ABORT:
            return;

          case StateReq::STOP:
            LOG_D("-> STOP <-");
            itemref_count    = -1;
            forget_retrieval = true;
            if (bitset != nullptr) {
              delete [] bitset;
              bitset = nullptr;
            }
            if (retriever_iddle) {
              mgr_queue_data.req = MgrReq::STOPPED;
              QUEUE_SEND(mgr_queue, mgr_queue_data, 0); 
            }
            else {
              stopping = true;
            }
            break;

          case StateReq::START_DOCUMENT:
            LOG_D("-> START_DOCUMENT <-");
            if (bitset) delete [] bitset;
            itemref_count = state_queue_data.itemref_count;
            bitset_size   = (itemref_count + 7) >> 3;
            bitset        = new uint8_t[bitset_size];
            if (bitset) {
              memset(bitset, 0, bitset_size);
              if (waiting_for_itemref == -1) {
                retrieve_queue_data.req           = RetrieveReq::RETRIEVE_ITEM;
                retrieve_queue_data.itemref_index = waiting_for_itemref =
                                                    state_queue_data.itemref_index;
                forget_retrieval                  = false;
                QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);
                LOG_D("Sent RETRIEVE_ITEM to retriever");
              }
              else {
                forget_retrieval    = true;
                next_itemref_to_get = state_queue_data.itemref_index;
              }
              retriever_iddle = false;
            }
            else {
              itemref_count    = -1;
              retriever_iddle  = true;
              forget_retrieval = true;
            }
            break;

          case StateReq::GET_ASAP:
            LOG_D("-> GET_ASAP <-");
            // Mgr request a specific item. If document retrieval not started, 
            // return a negative value.
            // If already done, let it know it a.s.a.p. If currently being processed,
            // keep a mark when it will be back. If not, queue the request.
            if (itemref_count == -1) {
              mgr_queue_data.req           = MgrReq::ASAP_READY;
              mgr_queue_data.itemref_index = -(state_queue_data.itemref_index + 1);
              QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              LOG_D("Sent ASAP_READY to Mgr");
            }
            else {
              int16_t itemref = state_queue_data.itemref_index;
              if ((bitset[itemref >> 3] & ( 1 << (itemref & 7))) != 0) {
                mgr_queue_data.req           = MgrReq::ASAP_READY;
                mgr_queue_data.itemref_index = itemref;
                QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
                LOG_D("Sent ASAP_READY to Mgr");
              }
              else if (waiting_for_itemref != -1) {
                asap_itemref = itemref;
              }
              else {
                asap_itemref                      = -1;
                waiting_for_itemref               = itemref;
                retrieve_queue_data.req           = RetrieveReq::GET_ASAP;
                retrieve_queue_data.itemref_index = itemref;
                retriever_iddle                   = false;
                QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);       
                LOG_D("Sent GET_ASAP to Retriever"); 
              }
            }
            break;

          // This is sent by the retrieval task, indicating that an item has been
          // processed.
          case StateReq::ITEM_READY:
            LOG_D("-> ITEM_READY <-");
            waiting_for_itemref = -1;
            if (itemref_count != -1) {
              int16_t itemref = -1;
              if (forget_retrieval) {
                forget_retrieval = false;
              }
              else {
                itemref = state_queue_data.itemref_index;
                if (itemref < 0) {
                  itemref = -(itemref + 1);
                  LOG_E("Unable to retrieve pages location for item %d", itemref);
                }
                bitset[itemref >> 3] |= (1 << (itemref & 7));
              }
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data.req = MgrReq::STOPPED;
                QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              }
              else {
                request_next_item(itemref);
              }
            }
            else {
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data.req = MgrReq::STOPPED;
                QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              }
            }
            break;

          // This is sent by the retrieval task, indicating that an ASAP item has been
          // processed.
          case StateReq::ASAP_READY:
            LOG_D("-> ASAP_READY <-");
            waiting_for_itemref = -1;
            if (itemref_count != -1) {
              int16_t itemref              = state_queue_data.itemref_index;
              mgr_queue_data.itemref_index = itemref;
              mgr_queue_data.req           = MgrReq::ASAP_READY;
              QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              LOG_D("Sent ASAP_READY to Mgr");
              if (itemref < 0) {
                itemref = -(itemref + 1);
                LOG_E("Unable to retrieve pages location for item %d", itemref);
              }
              bitset[itemref >> 3] |= (1 << (itemref & 7));
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data.req = MgrReq::STOPPED;
                QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              }
              else {
                request_next_item(itemref, true);
              }
            }
            else {
              if (stopping) {
                stopping = false;
                retriever_iddle  = true;
                mgr_queue_data.req = MgrReq::STOPPED;
                QUEUE_SEND(mgr_queue, mgr_queue_data, 0);
              }
            }
            break;
        }
      }
    }

    inline bool   retriever_is_iddle() { return retriever_iddle;  } 
    inline bool forgetting_retrieval() { return forget_retrieval; }

} state_task;

class RetrieverTask
{
  private:
    static constexpr const char * TAG = "RetrieverTask";

  public:
    void operator ()() const {
      RetrieveQueueData retrieve_queue_data;
      StateQueueData    state_queue_data;

      for (;;) {
        LOG_D("==> Waiting for request... <==");
        if (QUEUE_RECEIVE(retrieve_queue, retrieve_queue_data, portMAX_DELAY) == -1) {
          LOG_E("Receive error: %d: %s", errno, strerror(errno));
        }
        else {
          if (retrieve_queue_data.req == RetrieveReq::ABORT) return;

          LOG_D("-> %s <-", (retrieve_queue_data.req == RetrieveReq::GET_ASAP) ? "GET_ASAP" : "RETRIEVE_ITEM");

          LOG_D("Retrieving itemref --> %d <--", retrieve_queue_data.itemref_index);

          if (!page_locs.build_page_locs(retrieve_queue_data.itemref_index)) {
            // Unable to retrieve pages location for the requested index. Send back
            // a negative value to indicate the issue to the state task
            state_queue_data.itemref_index = -(retrieve_queue_data.itemref_index + 1);
          }
          else {
            state_queue_data.itemref_index = retrieve_queue_data.itemref_index;
          }

          //std::this_thread::sleep_for(std::chrono::seconds(5));
          state_queue_data.req = 
            (retrieve_queue_data.req == RetrieveReq::GET_ASAP) ? 
              StateReq::ASAP_READY : StateReq::ITEM_READY;

          QUEUE_SEND(state_queue, state_queue_data, 0);
          LOG_D("Sent %s to State", (state_queue_data.req == StateReq::ASAP_READY) ? "ASAP_READY" : "ITEM_READY");
        }
      }
    }
} retriever_task;

void
PageLocs::setup()
{
  #if EPUB_LINUX_BUILD
    mq_unlink("/mgr");
    mq_unlink("/state");
    mq_unlink("/retrieve");

    mgr_queue      = mq_open("/mgr",      O_RDWR|O_CREAT, S_IRWXU, &mgr_attr);
    if (mgr_queue == -1) { LOG_E("Unable to open mgr_queue: %d", errno); return; }

    state_queue    = mq_open("/state",    O_RDWR|O_CREAT, S_IRWXU, &state_attr);
    if (state_queue == -1) { LOG_E("Unable to open state_queue: %d", errno); return; }

    retrieve_queue = mq_open("/retrieve", O_RDWR|O_CREAT, S_IRWXU, &retrieve_attr);
    if (retrieve_queue == -1) { LOG_E("Unable to open retrieve_queue: %d", errno); return; }

    retriever_thread = std::thread(retriever_task);
    state_thread     = std::thread(state_task);
  #else
    mgr_queue      = xQueueCreate(5, sizeof(MgrQueueData));
    state_queue    = xQueueCreate(5, sizeof(StateQueueData));
    retrieve_queue = xQueueCreate(5, sizeof(RetrieveQueueData));

    auto cfg = create_config("retrieverTask", 0, 60 * 1024, configMAX_PRIORITIES - 2);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    retriever_thread = std::thread(retriever_task);
    
    cfg = create_config("stateTask", 0, 10 * 1024, configMAX_PRIORITIES - 2);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
    state_thread = std::thread(state_task);
  #endif
} 

void
PageLocs::abort_threads()
{
  RetrieveQueueData retrieve_queue_data;
  retrieve_queue_data.req = RetrieveReq::ABORT;
  LOG_D("abort_threads: Sending ABORT to Retriever");
  QUEUE_SEND(retrieve_queue, retrieve_queue_data, 0);

  retriever_thread.join();

  StateQueueData state_queue_data;
  state_queue_data.req = StateReq::ABORT;
  LOG_D("abort_threads: Sending ABORT to State");
  QUEUE_SEND(state_queue, state_queue_data, 0);

  state_thread.join();
}

bool 
PageLocs::page_locs_end_page(Page::Format & fmt)
{
  bool res = true;
  // if ((item_info.itemref_index == 0) || !page_out.is_empty()) {

    PageId   page_id   = PageId(item_info.itemref_index, start_of_page_offset);
    PageInfo page_info = PageInfo(current_offset - start_of_page_offset, -1);
    
    if (page_info.size > 0) {
      if ((item_info.itemref_index > 0) && (page_out.is_empty())) {
        page_info.size = -page_info.size; // The page will not be counted nor displayed
      }
      res = page_locs.insert(page_id, page_info);
    }
    // Gives the chance to book_viewer to show a page if required
    book_viewer.get_mutex().unlock();
    std::this_thread::yield();
    book_viewer.get_mutex().lock();

    // LOG_D("Page %d, offset: %d, size: %d", epub.get_page_count(), loc.offset, loc.size);
 
    SET_PAGE_TO_SHOW(epub.get_page_count()) // Debugging stuff
  //}

  start_of_page_offset = current_offset;

  page_out.start(fmt); // Start a new page

  return res;
}

bool
PageLocs::page_locs_recurse(pugi::xml_node node, Page::Format fmt)
{
  if (node == nullptr) return false;
  
  const char * name;
  const char * str = nullptr;
  std::string image_filename;

  image_filename.clear();

  Tags::iterator tag_it = tags.end();
  
  bool named_element = *(name = node.name()) != 0;

  if (named_element) { // A name is attached to the node.

    fmt.display = CSS::Display::INLINE;

    if ((tag_it = tags.find(name)) != tags.end()) {

      //LOG_D("==> %10s [%5d] %4d", name, current_offset, page_out.get_pos_y());

      switch (tag_it->second) {
        case Tag::BODY:
        case Tag::SPAN:
        case Tag::A:
          break;

      #if NO_IMAGE
        case IMG:
        case IMAGE:
          break;

      #else
        case Tag::IMG: 
          if (show_images) {
            xml_attribute attr = node.attribute("src");
            if (attr != nullptr) image_filename = attr.value();
            else current_offset++;
          }
          else {
            xml_attribute attr = node.attribute("alt");
            if (attr != nullptr) str = attr.value();
            else current_offset++;
          }
          break;

        case Tag::IMAGE: 
          if (show_images) {
            xml_attribute attr = node.attribute("xlink:href");
            if (attr != nullptr) image_filename = attr.value();
            else current_offset++;
          }
          break;

      #endif
        case Tag::PRE:
          fmt.pre     = start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::LI:
        case Tag::DIV:
        case Tag::BLOCKQUOTE:
        case Tag::P:
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::BREAK:
          SHOW_LOCATION("Page Break");
          if (!page_out.line_break(fmt)) {
            if (!page_locs_end_page(fmt)) return false;
            SHOW_LOCATION("Page Break");
            page_out.line_break(fmt);
          }
          current_offset++;
          break;

        case Tag::B:
        case Tag::STRONG: {
            Fonts::FaceStyle style = fmt.font_style;
            if      (style == Fonts::FaceStyle::NORMAL) style = Fonts::FaceStyle::BOLD;
            else if (style == Fonts::FaceStyle::ITALIC) style = Fonts::FaceStyle::BOLD_ITALIC;
            page_out.reset_font_index(fmt, style);
          }
          break;

        case Tag::I:
        case Tag::EM: {
            Fonts::FaceStyle style = fmt.font_style;
            if      (style == Fonts::FaceStyle::NORMAL) style = Fonts::FaceStyle::ITALIC;
            else if (style == Fonts::FaceStyle::BOLD  ) style = Fonts::FaceStyle::BOLD_ITALIC;
            page_out.reset_font_index(fmt, style);
          }
          break;

        case Tag::H1:
          fmt.font_size          = 1.25 * fmt.font_size;
          fmt.line_height_factor = 1.25 * fmt.line_height_factor;
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::H2:
          fmt.font_size          = 1.1 * fmt.font_size;
          fmt.line_height_factor = 1.1 * fmt.line_height_factor;
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::H3:
          fmt.font_size          = 1.05 * fmt.font_size;
          fmt.line_height_factor = 1.05 * fmt.line_height_factor;
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::H4:
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;

        case Tag::H5:
          fmt.font_size          = 0.8 * fmt.font_size;
          fmt.line_height_factor = 0.8 * fmt.line_height_factor;
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;
          
        case Tag::H6:
          fmt.font_size          = 0.7 * fmt.font_size;
          fmt.line_height_factor = 0.7 * fmt.line_height_factor;
          start_of_paragraph = true;
          fmt.display = CSS::Display::BLOCK;
          break;
      }
    }

    xml_attribute attr = node.attribute("style");
    CSS::Properties * element_properties = nullptr;
    if (attr) {
      const char * buffer = attr.value();
      const char * end    = buffer + strlen(buffer);
      element_properties  = CSS::parse_properties(&buffer, end, buffer);
    }

    page_out.adjust_format(node, fmt, element_properties, item_info.css); // Adjust format from element attributes

    if (element_properties) {
      CSS::clear_properties(element_properties);
      element_properties = nullptr;
    }

    if (fmt.display == CSS::Display::BLOCK) {
      if (page_out.some_data_waiting()) {
        SHOW_LOCATION("End Paragraph 3");
        if (!page_out.end_paragraph(fmt)) {
          if (!page_locs_end_page(fmt)) return false;

          if (page_out.some_data_waiting()) {
            SHOW_LOCATION("End Paragraph 4");
            page_out.end_paragraph(fmt);
          }
        }
      }
      SHOW_LOCATION("New Paragraph 4");
      if (!page_out.new_paragraph(fmt)) {
        if (!page_locs_end_page(fmt)) return false;
        SHOW_LOCATION("New Paragraph 5");
        page_out.new_paragraph(fmt);
      }
    } 
  }
  else {
    //This is a node inside a named node. It is contaning some text to show.
    str = fmt.pre ? node.text().get() : node.value();
  }

  if (show_images && !image_filename.empty()) {
    Page::Image image;
    if (page_out.get_image(image_filename, image)) {
      if (!page_out.add_image(image, fmt)) {
        if (!page_locs_end_page(fmt)) return false;
        if (start_of_paragraph) {
          SHOW_LOCATION("New Paragraph 3");
          page_out.new_paragraph(fmt);
          start_of_paragraph = false;
        }
        page_out.add_image(image, fmt);
      }
      stbi_image_free((void *) image.bitmap);
    }
    current_offset++;
  }

  if (str) {
    SHOW_LOCATION("->");
    while (*str) {
      if (uint8_t(*str) <= ' ') {
        if ((*str == ' ') || (!fmt.pre && (*str == '\n'))) {
          fmt.trim = !fmt.pre;
          if (!page_out.add_char(" ", fmt)) {
            if (!page_locs_end_page(fmt)) return false;
            if (start_of_paragraph) {
              SHOW_LOCATION("New Paragraph 6");
              page_out.new_paragraph(fmt, false);
            }
            else {
              SHOW_LOCATION("New Paragraph 7");
              page_out.new_paragraph(fmt, true);
            }
          }
        }
        else if (fmt.pre && (*str == '\n')) {
          page_out.line_break(fmt, 30);
        }
        str++;
        current_offset++; // Not an UTF-8, so it's ok...
      }
      else {
        const char * w = str;
        int32_t count = 0;
        while (uint8_t(*str) > ' ') { str++; count++; }
        std::string word;
        word.assign(w, count);
        if (!page_out.add_word(word.c_str(), fmt)) {
          if (!page_locs_end_page(fmt)) return false;
          if (start_of_paragraph) {
            SHOW_LOCATION("New Paragraph 8");
            page_out.new_paragraph(fmt, false);
          }
          else {
            SHOW_LOCATION("New Paragraph 9");
            page_out.new_paragraph(fmt, true);
          }
          page_out.add_word(word.c_str(), fmt);
        }
        current_offset += count;
        start_of_paragraph = false;
      }
    } 
  }

  if (named_element) {

    xml_node sub = node.first_child();
    while (sub) {
      if (!page_locs_recurse(sub, fmt)) return false;
      sub = sub.next_sibling();
    }

    if (fmt.display == CSS::Display::BLOCK) {
      if ((current_offset != start_of_page_offset) || page_out.some_data_waiting()) {
        SHOW_LOCATION("End Paragraph 5");
        if (!page_out.end_paragraph(fmt)) {
          if (!page_locs_end_page(fmt)) return false;
          if (page_out.some_data_waiting()) {
            SHOW_LOCATION("End Paragraph 6");
            page_out.end_paragraph(fmt);
          }
        }
      }
      start_of_paragraph = false;
    } 

    // In case that we are at the end of an html file and there remains
    // characters in the page pipeline, we call end_paragraph() to get them out on the page_out...
    if ((tag_it != tags.end()) && (tag_it->second == Tag::BODY)) {
      SHOW_LOCATION("End Paragraph 7");
      if (!page_out.end_paragraph(fmt)) {
        if (!page_locs_end_page(fmt)) return false;
        if (page_out.some_data_waiting()) {
          SHOW_LOCATION("End Paragraph 8");
          page_out.end_paragraph(fmt);
        }
      }
    }
  } 
  return true;
}

bool
PageLocs::build_page_locs(int16_t itemref_index)
{
  std::scoped_lock guard(book_viewer.get_mutex());

  TTF * font  = fonts.get(0);
  page_bottom = font->get_line_height(10) + (font->get_line_height(10) >> 1);
  
  page_out.set_compute_mode(Page::ComputeMode::LOCATION);

  show_images = current_format_params.show_images == 1;

  bool done = false;

  if (epub.get_item_at_index(itemref_index, item_info)) {

    int16_t idx;

    if ((idx = fonts.get_index("Fontbase", Fonts::FaceStyle::NORMAL)) == -1) {
      idx = 1;
    }
    
    int8_t font_size = current_format_params.font_size;

    int8_t show_title;
    config.get(Config::Ident::SHOW_TITLE, &show_title);

    int16_t top = show_title != 0 ? 30 : 10;

    Page::Format fmt = {
      .line_height_factor = 0.9,
      .font_index         = idx,
      .font_size          = font_size,
      .indent             = 0,
      .margin_left        = 0,
      .margin_right       = 0,
      .margin_top         = 0,
      .margin_bottom      = 0,
      .screen_left        = 10,
      .screen_right       = 10,
      .screen_top         = top,
      .screen_bottom      = page_bottom,
      .width              = 0,
      .height             = 0,
      .trim               = true,
      .pre                = false,
      .font_style         = Fonts::FaceStyle::NORMAL,
      .align              = CSS::Align::LEFT,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };

    while (!done) {

      current_offset       = 0;
      start_of_page_offset = 0;
      xml_node node = item_info.xml_doc.child("html");

      if (node && 
         (node = node.child("body"))) {

        page_out.start(fmt);

        if (!page_locs_recurse(node, fmt)) {
          LOG_D("html parsing issue or aborted by Mgr");
          break;
        }

        if (page_out.some_data_waiting()) page_out.end_paragraph(fmt);
      }
      else {
        LOG_D("No <body>");
        break;
      }

      page_locs_end_page(fmt);

      done = true;
    }
  }

  //page_out.set_compute_mode(Page::ComputeMode::DISPLAY);

  if (item_info.css != nullptr) {
    delete item_info.css;
    item_info.css = nullptr;
  }

  return done;
}

volatile bool relax = false;

bool 
PageLocs::retrieve_asap(int16_t itemref_index) 
{
  StateQueueData state_queue_data;
  state_queue_data.req = StateReq::GET_ASAP;
  state_queue_data.itemref_index = itemref_index;
  LOG_D("retrieve_asap: Sending GET_ASAP");
  QUEUE_SEND(state_queue, state_queue_data, 0);

  relax = true;
  MgrQueueData mgr_queue_data;
  LOG_D("==> Waiting for answer... <==");
  QUEUE_RECEIVE(mgr_queue, mgr_queue_data, portMAX_DELAY);
  LOG_D("-> %s <-", mgr_queue_data.req == MgrReq::ASAP_READY ? "ASAP_READY" : "ERROR!!!");
  relax = false;

  return true;
}

void
PageLocs::stop_document()
{
  StateQueueData state_queue_data;

  LOG_D("start_new_document: Sending STOP");
  state_queue_data.req = StateReq::STOP;
  QUEUE_SEND(state_queue, state_queue_data, 0);

  MgrQueueData mgr_queue_data;
  LOG_D("==> Waiting for STOPPED... <==");
  QUEUE_RECEIVE(mgr_queue, mgr_queue_data, portMAX_DELAY);
  LOG_D("-> %s <-", (mgr_queue_data.req == MgrReq::STOPPED) ? "STOPPED" : "ERROR!!!");
}

void 
PageLocs::start_new_document(int16_t count, int16_t itemref_index) 
{ 
  if (!state_task.retriever_is_iddle()) stop_document();

  check_for_format_changes(count, itemref_index, !load(epub.get_current_filename()));
}

bool 
PageLocs::insert(PageId & id, PageInfo & info) 
{
  if (!state_task.forgetting_retrieval()) {
    while (true) {
      if (relax) {
        // The page_locs class is still in control of the mutex, but is waiting
        // for the completion of an GET_ASAP item. As such, it is safe to insert
        // a new page info in the list.
        LOG_D("Relaxed page info insert...");
        pages_map.insert(std::make_pair(id, info));
        items_set.insert(id.itemref_index);
        break;
      }
      else {
        if (mutex.try_lock_for(std::chrono::milliseconds(10))) {
          pages_map.insert(std::make_pair(id, info));
          items_set.insert(id.itemref_index);
          mutex.unlock();
          break;
        }
      }
    }
    return true;
  }
  return false;
}

PageLocs::PagesMap::iterator 
PageLocs::check_and_find(const PageId & page_id) 
{
  PagesMap::iterator it = pages_map.find(page_id);
  if (!completed && (it == pages_map.end())) {
    if (retrieve_asap(page_id.itemref_index)) it = pages_map.find(page_id);
  }
  return it;
}

const PageLocs::PageId * 
PageLocs::get_next_page_id(const PageId & page_id, int16_t count) 
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it = check_and_find(page_id);
  if (it == pages_map.end()) {
    it = check_and_find(PageId(0,0));
  }
  else {
    PageId id = page_id;
    bool done = false;
    for (int16_t cptr = count; cptr > 0; cptr--) {
      PagesMap::iterator prev = it;
      do {
        id.offset += abs(it->second.size);
        it = pages_map.find(id);
        if (it == pages_map.end()) {
          id.itemref_index += 1; id.offset = 0;
          it = check_and_find(id);
          if (it == pages_map.end()) {
            it = (count > 1) ? prev : check_and_find(PageId(0,0));
            done = true;
          }
        }
      } while ((it->second.size < 0) && !done);
      if (done) break;
    }
  }
  return (it == pages_map.end()) ? nullptr : &it->first;
}

const PageLocs::PageId * 
PageLocs::get_prev_page_id(const PageId & page_id, int count) 
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it = check_and_find(page_id);
  if (it == pages_map.end()) {
    it = check_and_find(PageId(0, 0));
  }
  else {
    PageId id = it->first;
    
    bool done = false;
    for (int16_t cptr = count; cptr > 0; cptr--) {
      do {
        if (id.offset == 0) {
          if (id.itemref_index == 0) {
            if (count == 1) id.itemref_index = item_count - 1;
            else done = true;
          }
          else id.itemref_index--;

          if (items_set.find(id.itemref_index) == items_set.end()) {
            retrieve_asap(id.itemref_index);
          }
        }
        
        if (!done) {
          if (it == pages_map.begin()) it = pages_map.end();
          it--;
          id = it->first;
        }
      } while ((it->second.size < 0) && !done);
      if (done) break;
    }
  }
  return (it == pages_map.end()) ? nullptr : &it->first;
}

const PageLocs::PageId * 
PageLocs::get_page_id(const PageId & page_id) 
{
  std::scoped_lock guard(mutex);

  PagesMap::iterator it  = check_and_find(PageId(page_id.itemref_index, 0));
  PagesMap::iterator res = pages_map.end();
  while ((it != pages_map.end()) && (it->first.itemref_index == page_id.itemref_index)) {
    if ((it->first.offset <= page_id.offset) && ((it->first.offset + abs(it->second.size)) > page_id.offset)) { res = it; break; }
    it++;
  }
  return (res == pages_map.end()) ? nullptr : &res->first ;
}

void
PageLocs::computation_completed()
{
  std::scoped_lock guard(mutex);

  if (!completed) {
    int16_t page_nbr = 0;
    for (auto& entry : pages_map) {
      if (entry.second.size >= 0) entry.second.page_number = page_nbr++;
    }

    page_count = page_nbr;

    save(epub.get_current_filename());
  
    completed = true;
    event_mgr.set_stay_on(false);
  }
}

void
PageLocs::show()
{
  std::cout << "----- Page Locations -----" << std::endl;
  for (auto& entry : pages_map) {
    std::cout << " idx: " << entry.first.itemref_index
              << " off: " << entry.first.offset 
              << " siz: " << entry.second.size
              << " pg: "  << entry.second.page_number << std::endl;
  }
  std::cout << "----- End Page Locations -----" << std::endl;
}

void
PageLocs::check_for_format_changes(int16_t count, int16_t itemref_index, bool force)
{
  if (force || (memcmp(epub.get_book_format_params(), &current_format_params, sizeof(current_format_params)) != 0)) {

    LOG_D("==> Page locations recalc. <==");

    if (!state_task.retriever_is_iddle()) stop_document();

    clear();  

    current_format_params = *epub.get_book_format_params();

    item_count = count;
    StateQueueData state_queue_data;  

    state_queue_data.req           = StateReq::START_DOCUMENT;
    state_queue_data.itemref_count = item_count;
    state_queue_data.itemref_index = itemref_index;
    LOG_D("start_new_document: Sending START_DOCUMENT");
    QUEUE_SEND(state_queue, state_queue_data, 0);

    event_mgr.set_stay_on(true);
  }
}

bool PageLocs::load(const std::string & epub_filename)
{
  std::string   filename = epub_filename.substr(0, epub_filename.find_last_of('.')) + ".locs";
  std::ifstream file(filename, std::ios::in | std::ios::binary);

  LOG_D("Loading pages location from file %s.", filename.c_str());

  int8_t  version;
  int16_t pg_count;

  if (!file.is_open()) {
    LOG_E("Unable to open pages location file.");
    return false;
  }

  for (;;) {
    if (file.read(reinterpret_cast<char *>(&version), 1).fail()) break;
    if (version != LOCS_FILE_VERSION) break;

    if (file.read(reinterpret_cast<char *>(&current_format_params), sizeof(current_format_params)).fail()) break;
    if (file.read(reinterpret_cast<char *>(&pg_count),              sizeof(pg_count)           ).fail()) break;

    pages_map.clear();

    int16_t page_nbr = 0;

    for (int16_t i = 0; i < pg_count; i++) {
      PageId   page_id;
      PageInfo page_info;
      
      if (file.read(reinterpret_cast<char *>(&page_id.itemref_index), sizeof(page_id.itemref_index)).fail()) break;
      if (file.read(reinterpret_cast<char *>(&page_id.offset),        sizeof(page_id.offset       )).fail()) break;
      if (file.read(reinterpret_cast<char *>(&page_info.size),        sizeof(page_info.size       )).fail()) break;
      page_info.page_number = (page_info.size >= 0) ? page_nbr++ : -1;

      page_locs.insert(page_id, page_info);
    }

    page_count = page_nbr;

    break;
  }

  bool res = !file.fail();
  file.close();

  LOG_D("Page locations load %s.", res ? "Success" : "Error");

  completed = res;
  
  return res;
}

bool 
PageLocs::save(const std::string & epub_filename)
{
  std::string   filename = epub_filename.substr(0, epub_filename.find_last_of('.')) + ".locs";
  std::ofstream file(filename, std::ios::out | std::ios::binary);

  LOG_D("Saving pages location to file %s", filename.c_str());

  if (!file.is_open()) {
    LOG_E("Not able to open pages location file.");
    return false;
  }

  int16_t page_count = pages_map.size();

  for (;;) {
    if (file.write(reinterpret_cast<const char *>(&LOCS_FILE_VERSION),     1                            ).fail()) break;
    if (file.write(reinterpret_cast<const char *>(&current_format_params), sizeof(current_format_params)).fail()) break;
    if (file.write(reinterpret_cast<const char *>(&page_count),            sizeof(page_count)           ).fail()) break;

    for (auto & page : pages_map) {
      if (file.write(reinterpret_cast<const char *>(&page.first.itemref_index), sizeof(page.first.itemref_index)).fail()) break;
      if (file.write(reinterpret_cast<const char *>(&page.first.offset),        sizeof(page.first.offset       )).fail()) break;
      if (file.write(reinterpret_cast<const char *>(&page.second.size),         sizeof(page.second.size        )).fail()) break;
    }

    break;
  }

  bool res = !file.fail();
  file.close();

  LOG_D("Page locations save %s.", res ? "Success" : "Error");

  return res;
}