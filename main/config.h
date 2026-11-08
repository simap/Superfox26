// The data partition: a LittleFS volume at CFG_BASE with the fonts (fonts/sans.ttf, bold.ttf,
// mono.ttf, from the partition image) and the page list in urls.txt, written by the console.
// One page per line: URL, then optionally the scroll offset in pixels and the seconds the page
// stays up. Without urls.txt the list is PAGE_URLS from urls.h. The list is cached in RAM behind
// a mutex, so the console and the browser task can use it from different tasks.
#pragma once

#include <string>
#include <vector>

#define CFG_BASE "/data"

struct page_entry
{
    std::string url;
    int         offset  = 0; // first document row shown, clamped to the document height at render
    int         seconds = 0; // time from the start of this page to the next, 0 for CONFIG_B26_CYCLE_SECONDS
};

// Mounts the partition, formatting it when it is blank, and loads the list. False when the
// partition is missing; the default list is still available.
bool cfg_init();

// Copy of the current page list.
std::vector<page_entry> cfg_urls();

// True when the list came from urls.txt rather than urls.h.
bool cfg_urls_from_file();

// Replace the list and write it. False when the write failed; the RAM copy is replaced anyway.
bool cfg_urls_set(const std::vector<page_entry>& pages);

// Delete urls.txt and go back to the defaults.
bool cfg_urls_reset();
