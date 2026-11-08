// The page cycle: a task on core 1 that renders the next page at its scroll offset, refreshes
// the panel, and holds it for the page's seconds (CONFIG_B26_CYCLE_SECONDS when unset). Waits
// for WiFi and a non-empty list when either is missing.
#pragma once

// Starts the task. epd_begin, render_init, cfg_init and wifi_init must have run.
bool browser_start();

// Ends the current wait and starts the next page.
void browser_next();

// Index into the URL list of the page being shown or rendered.
int browser_page_index();

// One word: "wait", "nowifi", "nourls", "render", "refresh".
const char* browser_state();
