#define __BOOK_CONTROLLER__ 1
#include "book_controller.hpp"

#include "app_controller.hpp"
#include "book_view.hpp"
#include "epub.hpp"
#include "page.hpp"

#include "rapidxml.hpp"

#include <string>

static const char * TAG = "BookController";

void 
BookController::enter()
{
  book_view.show_page(current_page);
}

void 
BookController::leave()
{

}

bool
BookController::open_book_file(std::string & book_filename, int16_t book_idx)
{
  if (epub.open_file(book_filename)) {
    epub.retrieve_page_locs(book_idx);
    current_page = 0;
    return true;
  }
  return false;
}

void 
BookController::key_event(EventMgr::KeyEvent key)
{
  switch (key) {
    case EventMgr::KEY_LEFT:
      if (current_page > 0) {
        book_view.show_page(--current_page);
      }
      break;
    case EventMgr::KEY_UP:
      current_page -= 10;
      if (current_page < 0) current_page = 0;
      book_view.show_page(current_page);
      break;
    case EventMgr::KEY_RIGHT:
      current_page += 1;
      if (current_page >= epub.get_page_count()) {
        current_page = epub.get_page_count() - 1;
      }
      book_view.show_page(current_page);
      break;
    case EventMgr::KEY_DOWN:
      current_page += 10;
      if (current_page >= epub.get_page_count()) {
        current_page = epub.get_page_count() - 1;
      }
      book_view.show_page(current_page);
      break;
    case EventMgr::KEY_SELECT: {
        for (int i = 0; i < epub.get_page_count(); i++) {
          current_page = i;
          book_view.show_page(i);
        }
      }
      break;
    case EventMgr::KEY_HOME:
      app_controller.set_controller(AppController::PARAM);
      break;
  }
}