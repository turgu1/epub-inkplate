// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <string>

#include "strlcpy.hpp"

#define APP_VERSION "1.2.0"

#if !(defined(EPUB_LINUX_BUILD) || defined(EPUB_INKPLATE_BUILD)) 
  #error "BUILD_TYPE Not Set."
#else
  #if !(EPUB_LINUX_BUILD || EPUB_INKPLATE_BUILD)
    #error "One of EPUB_LINUX_BUILD or EPUB_INKPLATE_BUILD must be set to 1"
  #endif
#endif

#define USE_EPUB_FONTS 1  ///< 1: Embeded fonts in EPub books are loaded and used 0: Only preset fonts are used

#if EPUB_LINUX_BUILD
  #define MAIN_FOLDER "/home/turgu1/Dev/EPub-InkPlate/SDCard"
#else
  #define MAIN_FOLDER "/sdcard"
#endif

#define FONTS_FOLDER MAIN_FOLDER "/fonts"
#define BOOKS_FOLDER MAIN_FOLDER "/books"

#ifndef DEBUGGING
  #define DEBUGGING 0
#endif

// Debugging aid
//
// These are simple tools to show information about formatting for a specific page
// in a book being tested. The problems are often reflected through bad offsets
// computation for the beginning or/and end of pages.
//
// To get good results for the user, pages location offsets computed and put in the books 
// database must be the same as the one found when the book is being displayed. 
// If not, lines at the beginning or the end of a page can be duplicated or 
// part of a paragraph can be lost.
//
// To use, set DEBUGGING_AID to 1 and select the page number (0 = first page), 
// setting the PAGE_TO_SHOW_LOCATION. At
// pages location refresh time (done by the book_dir class at start time), 
// a lot of data will be output on the terminal. Then, opening the book,
// and going to the selected page, some similar information will be sent to 
// the terminal. You can then compare the results and find the differences.
//
// To be used, only one book must be in the books folder and the books_dir.db 
// file must be deleted before any trial.

#define DEBUGGING_AID  0   ///< 1: Allow for specific page debugging output

#if DEBUGGING_AID
  #define PAGE_TO_SHOW_LOCATION 2
  #define SET_PAGE_TO_SHOW(val) { show_location = val == PAGE_TO_SHOW_LOCATION; }
  #define SHOW_LOCATION(msg) { if (show_location) { \
    std::cout << msg << " Offset:" << current_offset << " "; \
    page.show_controls("  "); \
    std::cout << "     "; \
    page.show_fmt(fmt, "  ");  }}

  #if __GLOBAL__
    bool show_location = false;
  #else
    extern bool show_location;
  #endif
#else
  #define SET_PAGE_TO_SHOW(val)
  #define SHOW_LOCATION(msg)
#endif

// The following data definitions are here for laziness... 
// ToDo: Move them to the appropriate location

struct Dim { 
  uint16_t width; 
  uint16_t height; 
  Dim(uint16_t w, uint16_t h) { 
    width = w; 
    height = h;
  }
  Dim() {}
};

struct Pos { 
  int16_t x; 
  int16_t y; 
  Pos(int16_t xpos, int16_t ypos) { 
    x = xpos; y = ypos; 
  }
  Pos() {}
};

// BREAK is BR... A defined BR in esp-idf is the cause of this!!
enum class Tag { BODY, P, LI, BREAK, H1, H2, H3, H4, H5, H6, 
                 B, I, A, IMG, IMAGE, EM, DIV, SPAN, PRE,
                 BLOCKQUOTE, STRONG };

typedef std::unordered_map<std::string, Tag> Tags;

#if __GLOBAL__
  Tags tags
  = {{"p",           Tag::P}, {"div",     Tag::DIV}, {"span", Tag::SPAN}, {"br",  Tag::BREAK}, {"h1",                 Tag::H1},  
     {"h2",         Tag::H2}, {"h3",       Tag::H3}, {"h4",     Tag::H4}, {"h5",     Tag::H5}, {"h6",                 Tag::H6}, 
     {"b",           Tag::B}, {"i",         Tag::I}, {"em",     Tag::EM}, {"body", Tag::BODY}, {"a",                   Tag::A},
     {"img",       Tag::IMG}, {"image", Tag::IMAGE}, {"li",     Tag::LI}, {"pre",   Tag::PRE}, {"blockquote", Tag::BLOCKQUOTE},
     {"strong", Tag::STRONG}}
  ;
#else
  extern Tags tags;
#endif
