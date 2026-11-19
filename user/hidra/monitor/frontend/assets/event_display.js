/* Browse a frozen ADC/FERS batch entirely in this browser. No backend lookups. */
(function () {
    "use strict";
    window.dash_clientside = window.dash_clientside || {};
    window.dash_clientside.hidra_event_display = {
        select_event: function (batch, previous, next, selected, collection, captured) {
            const dc = window.dash_clientside;
            const unchanged = dc.no_update;
            const context = dc.callback_context || {};
            const trigger = context.triggered_id || ((context.triggered || [])[0] || {}).prop_id?.split(".")[0] || "";
            const isBatch = trigger.endsWith("-event-batch") || (!collection && batch);
            const valid = (event) => event && typeof event.id === "string" && Array.isArray(event.adc);
            let events = collection?.events || [];
            let newest = null;
            let changed = false;

            if (isBatch && batch && Array.isArray(batch.events)) {
                // Backend bounds batches to 100; keep one earlier selection too.
                events = batch.events.filter(valid).slice(-100);
                newest = events.length ? events[events.length - 1].id : null;
                changed = true;
            }
            if (valid(captured) && !events.some((event) => event.id === captured.id)) {
                events = [captured, ...events];
                changed = true;
            }

            let chosen = selected;
            const ids = events.map((event) => event.id);
            if (isBatch && batch?.action === "latest" && newest) {
                chosen = newest;
            } else if (!ids.includes(chosen)) {
                chosen = valid(captured) && ids.includes(captured.id) ? captured.id : ids[ids.length - 1];
            }
            const goPrevious = trigger.endsWith("-event-previous") && Number(previous) > 0;
            const goNext = trigger.endsWith("-event-next") && Number(next) > 0;
            if (goPrevious || goNext) {
                const step = goPrevious ? -1 : 1;
                chosen = ids[Math.max(0, Math.min(ids.length - 1, ids.indexOf(chosen) + step))];
            }

            const snapshot = events.find((event) => event.id === chosen);
            const label = (event) => {
                const kinds = {0: "gate", 1: "physics", 2: "pedestal", 3: "physics + pedestal"};
                return `run ${event.run_number} · event ${event.event_number} · ${kinds[event.trigger_mask] || "unknown trigger"}`;
            };
            const options = changed ? [...events].reverse().map((event) => ({label: label(event), value: event.id})) : unchanged;
            const metadata = snapshot ? `${label(snapshot)} · trigger ${snapshot.trigger_number ?? "—"} · ` +
                `spill ${snapshot.spill_number ?? "—"} · timestamp ${snapshot.timestamp_begin_ns ?? "—"} ns` :
                "No loaded events. Press Refresh events after acquisition starts.";
            return [
                changed ? {events: events} : unchanged,
                options,
                (chosen || null) === (selected || null) ? unchanged : (chosen || null),
                snapshot && snapshot.id !== captured?.id ? snapshot : unchanged,
                metadata
            ];
        }
    };
}());
