/* Run with node tests/event_display_selection_test.js; no npm dependencies. */
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const unchanged = Symbol("no_update");
const window = {dash_clientside: {no_update: unchanged, callback_context: {}}};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../assets/event_display.js"), "utf8"), {window});
const select = window.dash_clientside.hidra_event_display.select_event;
const event = (id, number = 1) => ({id, run_number: 42, event_number: number,
    trigger_number: number, trigger_mask: 1, timestamp_begin_ns: "1795000000123456789",
    adc: [0, null, 123], fers_hg: [12, 0], fers_lg: [3, 4]});
function call(trigger, batch, chosen, collection, captured) {
    // The selector must work with no fetch/network API at all.
    window.dash_clientside.callback_context = {triggered: [{prop_id: "event_display__p0-event-" + trigger + ".value"}]};
    return select(batch, trigger === "previous" ? 1 : 0, trigger === "next" ? 1 : 0, chosen, collection, captured);
}

const first = event("one", 100), second = event("two", 110), third = event("three", 200);
const batch = {revision: "a", action: "refresh", events: [first, second, third]};
let result = call("batch", batch, null, null, null);
assert.equal(result[2], "three");
assert.equal(result[3], third);
assert.match(result[4], /event 200/);
const collection = result[0];
assert.equal(collection.events.length, 3);

// Simulate complete backend eviction/reset. Every listed event remains available.
batch.events = [];
result = call("select", batch, "one", collection, third);
assert.equal(result[3], first);
assert.equal(result[3].adc[0], 0);
assert.equal(result[3].adc[1], null);
assert.equal(call("next", batch, "one", collection, first)[3], second);
assert.equal(call("previous", batch, "two", collection, second)[3], first);
assert.equal(call("previous", batch, "one", collection, first)[3], unchanged);
assert.equal(call("select", batch, "two", collection, second)[3], unchanged);

// Refresh preserves the old selection, while Latest explicitly switches it.
const fresh = {revision: "b", action: "refresh", events: [event("new", 300)]};
// Mounting a button with n_clicks=0 must not act like a user navigation click.
window.dash_clientside.callback_context = {triggered_id: "event_display__p0-event-next"};
const mounted = select(fresh, 0, 0, "one", null, first);
assert.equal(mounted[2], unchanged);
assert.equal(mounted[3], unchanged);
result = call("batch", fresh, "one", collection, first);
assert.equal(result[2], unchanged);
assert.equal(result[3], unchanged);
assert.equal(result[0].events[0], first);
assert.equal(result[1].length, 2);
fresh.action = "latest";
result = call("batch", fresh, "one", collection, first);
assert.equal(result[2], "new");
assert.equal(result[3].event_number, 300);
assert.equal(collection.events[0], first); // Other viewers' groups are unchanged.

// An empty new run or unavailable backend cannot invalidate an already loaded event.
result = call("batch", {action: "refresh", events: []}, "one", collection, first);
assert.equal(result[0].events[0], first);
assert.equal(result[3], unchanged);
result = call("batch", null, "one", collection, first);
assert.equal(result[0], unchanged);
assert.equal(result[3], unchanged);

// Bounded browser memory even with an unexpectedly large response, plus one selection.
const huge = {action: "latest", events: Array.from({length: 500}, (_, i) => event("large" + i, i))};
result = call("batch", huge, "one", collection, first);
assert.equal(result[0].events.length, 101);
assert.equal(result[3].event_number, 499);
assert.equal(call("batch", null, null, null, null)[2], unchanged);
console.log("Browser-local navigation, frozen groups, gain payloads, reset/offline and memory bounds passed.");
