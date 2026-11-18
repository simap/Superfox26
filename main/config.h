// Holds the list of pages the browser steps through, and stores edits to that list on the data
// partition so a changed list survives a restart.
// When no stored list is present, a built-in list is used.
// Call cfg_init once before any other function here.
#pragma once

#include <string>
#include <vector>

#define DATA_BASE "/data"

struct page_entry
{
    std::string url;
    int         offset  = 0;
    int         seconds = 0;
};

bool cfg_init();

std::vector<page_entry> cfg_urls();

bool cfg_urls_from_file();

bool cfg_urls_set(const std::vector<page_entry>& pages);

bool cfg_urls_reset();
