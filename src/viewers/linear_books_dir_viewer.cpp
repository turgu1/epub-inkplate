// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __LINEAR_BOOKS_DIR_VIEWER__ 1
#include "viewers/linear_books_dir_viewer.hpp"

#include "models/fonts.hpp"
#include "models/config.hpp"
#include "viewers/page.hpp"

#if EPUB_INKPLATE_BUILD
  #include "viewers/battery_viewer.hpp"
#endif

#include "screen.hpp"

#include <iomanip>

void
LinearBooksDirViewer::setup()
{
  books_per_page = (Screen::HEIGHT - FIRST_ENTRY_YPOS - 20 + SPACE_BETWEEN_ENTRIES) / 
                   (BooksDir::max_cover_height + SPACE_BETWEEN_ENTRIES);
  page_count = (books_dir.get_book_count() + books_per_page - 1) / books_per_page;

  current_page_nbr = -1;
  current_book_idx = -1;
  current_item_idx = -1;

  LOG_D("Books count: %d", books_dir.get_book_count());
}

void 
LinearBooksDirViewer::show_page(int16_t page_nbr, int16_t hightlight_item_idx)
{
  current_page_nbr = page_nbr;
  current_item_idx = hightlight_item_idx;

  int16_t book_idx = page_nbr * books_per_page;
  int16_t last     = book_idx + books_per_page;

  page.set_compute_mode(Page::ComputeMode::DISPLAY);

  if (last > books_dir.get_book_count()) last = books_dir.get_book_count();

  int16_t xpos = 20 + BooksDir::max_cover_width;
  int16_t ypos = FIRST_ENTRY_YPOS;

  Page::Format fmt = {
      .line_height_factor =   0.8,
      .font_index         = TITLE_FONT,
      .font_size          = TITLE_FONT_SIZE,
      .indent             =     0,
      .margin_left        =     0,
      .margin_right       =     0,
      .margin_top         =     0,
      .margin_bottom      =     0,
      .screen_left        =  xpos,
      .screen_right       =    10,
      .screen_top         =  ypos,
      .screen_bottom      = (int16_t)(Screen::HEIGHT - (ypos + BooksDir::max_cover_width + 20)),
      .width              =     0,
      .height             =     0,
      .trim               =  true,
      .pre                = false,
      .font_style         = Fonts::FaceStyle::NORMAL,
      .align              = CSS::Align::LEFT,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };

  page.start(fmt);

  for (int item_idx = 0; book_idx < last; item_idx++, book_idx++) {

    int16_t top_pos = ypos;

    const BooksDir::EBookRecord * book = books_dir.get_book_data(book_idx);

    if (book == nullptr) break;
    
    Page::Image image = (Page::Image) {
      .bitmap = (uint8_t *) book->cover_bitmap,
      .dim    = Dim(book->cover_width, book->cover_height)
    };
    page.put_image(image, Pos(10 + books_dir.MAX_COVER_WIDTH - book->cover_width, ypos));

    #if !(INKPLATE_6PLUS || TOUCH_TRIAL)
      if (item_idx == current_item_idx) {
        page.put_highlight(Dim(Screen::WIDTH - (25 + BooksDir::max_cover_width), BooksDir::max_cover_height), 
                           Pos(xpos - 5, ypos));
      }
    #endif

    fmt.font_index    = TITLE_FONT;
    fmt.font_size     = TITLE_FONT_SIZE;
    fmt.font_style    = Fonts::FaceStyle::NORMAL;
    fmt.screen_top    = ypos,
    fmt.screen_bottom = (int16_t)(Screen::HEIGHT - (ypos + BooksDir::max_cover_height + SPACE_BETWEEN_ENTRIES)),

    page.set_limits(fmt);
    page.new_paragraph(fmt);
    page.add_text(book->title, fmt);
    page.end_paragraph(fmt);

    fmt.font_index = AUTHOR_FONT;
    fmt.font_size  = AUTHOR_FONT_SIZE;
    fmt.font_style = Fonts::FaceStyle::ITALIC;

    page.new_paragraph(fmt);
    page.add_text(book->author, fmt);
    page.end_paragraph(fmt);

    ypos = top_pos + BooksDir::max_cover_height + SPACE_BETWEEN_ENTRIES;
  }

  TTF * font = fonts.get(0);

  fmt.line_height_factor = 1.0;
  fmt.font_index         = PAGENBR_FONT;
  fmt.font_size          = PAGENBR_FONT_SIZE;
  fmt.font_style         = Fonts::FaceStyle::NORMAL;
  fmt.align              = CSS::Align::CENTER;
  fmt.screen_left        = 10;
  fmt.screen_right       = 10; 
  fmt.screen_top         = 10;
  fmt.screen_bottom      = 10;

  page.set_limits(fmt);

  std::ostringstream ostr;
  ostr << page_nbr + 1 << " / " << page_count;

  page.put_str_at(ostr.str(), Pos(-1, Screen::HEIGHT + font->get_descender_height(PAGENBR_FONT_SIZE) - 2), fmt);

  #if EPUB_INKPLATE_BUILD
    int8_t show_heap;
    config.get(Config::Ident::SHOW_HEAP, &show_heap);

    if (show_heap != 0) {
      ostr.str(std::string());
      ostr << heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) 
            << " / " 
            << heap_caps_get_free_size(MALLOC_CAP_8BIT);
      fmt.align = CSS::Align::RIGHT;
      page.put_str_at(ostr.str(), Pos(-1, Screen::HEIGHT + font->get_descender_height(PAGENBR_FONT_SIZE) - 2), fmt);
    }

    BatteryViewer::show();
  #endif

  page.paint();
}

void 
LinearBooksDirViewer::highlight(int16_t item_idx)
{
  #if !(INKPLATE_6PLUS || TOUCH_TRIAL)
  page.set_compute_mode(Page::ComputeMode::DISPLAY);

  if (current_item_idx != item_idx) {

    // Clear the highlighting of the current item

    int16_t book_idx = current_page_nbr * books_per_page + current_item_idx;

    int16_t xpos = 20 + BooksDir::max_cover_width;
    int16_t ypos = FIRST_ENTRY_YPOS + (current_item_idx * (BooksDir::max_cover_height + SPACE_BETWEEN_ENTRIES));

    const BooksDir::EBookRecord * book = books_dir.get_book_data(book_idx);

    if (book == nullptr) return;

    // TTF * font = fonts.get(1, 9);

    Page::Format fmt = {
      .line_height_factor = 0.8,
      .font_index         = TITLE_FONT,
      .font_size          = TITLE_FONT_SIZE,
      .indent             = 0,
      .margin_left        = 0,
      .margin_right       = 0,
      .margin_top         = 0,
      .margin_bottom      = 0,
      .screen_left        = xpos,
      .screen_right       = 10,
      .screen_top         = ypos,
      .screen_bottom      = (int16_t)(Screen::HEIGHT - (ypos + BooksDir::max_cover_width + 20)),
      .width              = 0,
      .height             = 0,
      .trim               = true,
      .pre                = false,
      .font_style         = Fonts::FaceStyle::NORMAL,
      .align              = CSS::Align::LEFT,
      .text_transform     = CSS::TextTransform::NONE,
      .display            = CSS::Display::INLINE
    };

    page.start(fmt);

    page.clear_highlight(
      Dim(Screen::WIDTH - (25 + BooksDir::max_cover_width), BooksDir::max_cover_height),
      Pos(xpos - 5, ypos));

    page.set_limits(fmt);
    page.new_paragraph(fmt);
    page.add_text(book->title, fmt);
    page.end_paragraph(fmt);

    fmt.font_index = AUTHOR_FONT;
    fmt.font_size  = AUTHOR_FONT_SIZE;
    fmt.font_style = Fonts::FaceStyle::ITALIC;

    page.new_paragraph(fmt);
    page.add_text(book->author, fmt);
    page.end_paragraph(fmt);
    
    // Highlight the new current item

    current_item_idx = item_idx;

    book_idx = current_page_nbr * books_per_page + current_item_idx;
    ypos = FIRST_ENTRY_YPOS + (current_item_idx * (BooksDir::max_cover_height + 6));

    book = books_dir.get_book_data(book_idx);

    if (book == nullptr) return;
    
    page.put_highlight(
      Dim(Screen::WIDTH - (25 + BooksDir::max_cover_width), BooksDir::max_cover_height),
      Pos(xpos - 5, ypos));


    fmt.font_index    = TITLE_FONT,
    fmt.font_size     = TITLE_FONT_SIZE,
    fmt.font_style    = Fonts::FaceStyle::NORMAL,
    fmt.screen_top    = ypos,
    fmt.screen_bottom = (int16_t)(Screen::HEIGHT - (ypos + BooksDir::max_cover_width + 20)),

    page.set_limits(fmt);
    page.new_paragraph(fmt);
    page.add_text(book->title, fmt);
    page.end_paragraph(fmt);

    fmt.font_index = AUTHOR_FONT,
    fmt.font_size  = AUTHOR_FONT_SIZE,
    fmt.font_style = Fonts::FaceStyle::ITALIC,

    page.new_paragraph(fmt);
    page.add_text(book->author, fmt);
    page.end_paragraph(fmt);

    #if EPUB_INKPLATE_BUILD
      BatteryViewer::show();
    #endif

    page.paint(false);
  }
  #endif
}

int16_t
LinearBooksDirViewer::show_page_and_highlight(int16_t book_idx)
{
  int16_t page_nbr = book_idx / books_per_page;
  int16_t item_idx = book_idx % books_per_page;

  if (current_page_nbr != page_nbr) {
    show_page(page_nbr, item_idx);
  }
  else {
    if (item_idx != current_item_idx) highlight(item_idx);
  }
  current_book_idx = book_idx;
  return current_book_idx;
}

void
LinearBooksDirViewer::highlight_book(int16_t book_idx)
{
  highlight(book_idx % books_per_page);  
  current_book_idx = book_idx;
}

int16_t
LinearBooksDirViewer::next_page()
{
  return next_column();
}

int16_t
LinearBooksDirViewer::prev_page()
{
  return prev_column();
}

int16_t
LinearBooksDirViewer::next_item()
{
  int16_t book_idx = current_book_idx + 1;
  if (book_idx >= books_dir.get_book_count()) {
    book_idx = books_dir.get_book_count() - 1;
  }
  return show_page_and_highlight(book_idx);
}

int16_t
LinearBooksDirViewer::prev_item()
{
  int16_t book_idx = current_book_idx - 1;
  if (book_idx < 0) book_idx = 0;
  return show_page_and_highlight(book_idx);
}

int16_t
LinearBooksDirViewer::next_column()
{
  int16_t book_idx = current_book_idx + books_per_page;
  if (book_idx >= books_dir.get_book_count()) {
    book_idx = books_dir.get_book_count() - 1;
  }
  else {
    book_idx = (book_idx / books_per_page) * books_per_page;
  }
  return show_page_and_highlight(book_idx);
}

int16_t
LinearBooksDirViewer::prev_column()
{
  int16_t book_idx = current_book_idx - books_per_page;
  if (book_idx < 0) book_idx = 0;
  else book_idx = (book_idx / books_per_page) * books_per_page;
  return show_page_and_highlight(book_idx);
}
