#!/usr/bin/env python3
"""End-to-end test of the browser UI in a real headless browser.

Loads ui/index.html (the real C++ engine compiled to WebAssembly), drives it programmatically (sends orders,
crossing trades, the -50% and +1 tick modify buttons, hover queue tooltips, post-only rejection, random flow,
reset, the benchmark button, tab switching, and that every figure on the real-data tab loads), then reads the
result back out of the DOM.

    python scripts/ui_e2e.py            # needs Chrome, Chromium or Edge (or set CHROME=/path/to/browser)

Under headless Chrome's virtual clock the in-page benchmark reports 0 (time does not advance); the engine's
speed is measured by wasm/test_wasm.js instead.
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

test = r'''
<pre id="e2e" style="white-space:pre-wrap"></pre>
<script>
const out = []; const ok = (c, m) => out.push((c ? "PASS " : "FAIL ") + m);
window.addEventListener("error", e => out.push("FAIL uncaught error: " + e.message));
function click(id) { document.getElementById(id).click(); }
function wait(ms) { return new Promise(r => setTimeout(r, ms)); }
(async () => {
  for (let i = 0; i < 100 && !book; i++) await wait(50);
  ok(!!book, "engine loaded");
  ok(document.getElementById("engine-badge").textContent.includes("WebAssembly"), "badge says WebAssembly");
  ok(document.querySelectorAll("#asks .row").length === 6 && document.querySelectorAll("#bids .row").length === 6, "seeded ladder shows 6+6 levels");
  ok(document.getElementById("k-spread").textContent === "2", "spread is 2 ticks, got " + document.getElementById("k-spread").textContent);
  // limit buy that crosses: 1001 buy 150 should hit the 100 @1001 ask and rest 50
  document.getElementById("f-price").value = 1001; document.getElementById("f-qty").value = 150;
  click("b-buy"); await wait(50);
  ok(document.getElementById("k-trades").textContent === "1", "one trade after crossing buy");
  ok(/1 order|filled immediately/.test(document.getElementById("msg").textContent) && document.getElementById("msg").textContent.includes("100 lots filled"), "message reports 100 filled: " + document.getElementById("msg").textContent);
  ok(document.querySelectorAll("#mine .o").length === 1, "one resting order of mine listed");
  // modify buttons
  document.querySelector('#mine button[data-act="half"]').click(); await wait(20);
  ok(/25 @ 1001|#\d+ buy 25 @ 1001/.test(document.getElementById("mine").textContent), "-50% halves my order: " + document.getElementById("mine").textContent.trim());
  document.querySelector('#mine button[data-act="tick"]').click(); await wait(20);
  ok(document.querySelectorAll("#mine .o").length === 0 && book.trades === 2, "+1 tick reprices onto the ask at 1002 and trades immediately (trades=" + book.trades + ")");
  // hover shows the queue
  const row = document.querySelector('#bids .row'); row.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));
  ok(row.title.includes("#") , "hover tooltip lists the queue: " + JSON.stringify(row.title));
  // market order and post-only reject
  document.getElementById("f-type").value = "market"; document.getElementById("f-type").onchange(); document.getElementById("f-qty").value = 10; click("b-sell"); await wait(20);
  ok(document.getElementById("msg").textContent.includes("filled immediately"), "market sell executes");
  document.getElementById("f-type").value = "limit"; document.getElementById("f-type").onchange();
  document.getElementById("f-post").checked = true; document.getElementById("f-price").value = 1100; document.getElementById("f-qty").value = 10; click("b-buy"); await wait(20);
  ok(document.getElementById("msg").textContent.includes("post-only"), "post-only crossing order rejected: " + document.getElementById("msg").textContent);
  // random flow ticks and invariants
  click("b-flow"); await wait(1500); click("b-flow");
  ok(book.orderCount() > 10 && book.trades > 1, "random flow produced trades, orders=" + book.orderCount() + " trades=" + book.trades);
  click("b-reset"); await wait(50);
  ok(document.getElementById("k-trades").textContent === "0", "reset clears the trade counter");
  click("b-bench"); await wait(4000);
  const b = document.getElementById("bench-out").textContent; ok(/M orders\/s/.test(b), "benchmark button ran: " + b);
  document.querySelector('.tab[data-tab="results"]').click();
  ok(document.querySelectorAll("#ch-thr .chart-row").length === 5, "results charts rendered");
  document.querySelector('.tab[data-tab="real"]').click(); await wait(200);
  ok(!document.getElementById("tab-real").classList.contains("hidden") && document.getElementById("tab-book").classList.contains("hidden"), "real-data tab shows and hides the others");
  ok(document.querySelectorAll("#tab-real .fig img").length === 6, "six figures present on the real-data tab");
  const imgs = [...document.querySelectorAll("#tab-real .fig img")];
  await Promise.all(imgs.map(i => i.complete ? 0 : new Promise(r => { i.onload = r; i.onerror = r; })));
  ok(imgs.every(i => i.naturalWidth > 100), "all six figures load (paths resolve): " + imgs.map(i => i.naturalWidth).join(","));
  document.getElementById("e2e").textContent = out.join("\n") + "\n" + (out.some(l => l.startsWith("FAIL")) ? "RESULT: FAIL" : "RESULT: ALL PASS");
})();
</script>
'''


def find_browser():
    if os.environ.get("CHROME"):
        return os.environ["CHROME"]
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome", "msedge"):
        p = shutil.which(name)
        if p:
            return p
    for p in (r"C:/Program Files/Google/Chrome/Application/chrome.exe",
              r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
              "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"):
        if os.path.exists(p):
            return p
    sys.exit("no browser found; set CHROME=/path/to/chrome")


def main():
    tmp = tempfile.mkdtemp()
    try:
        shutil.copytree(os.path.join(ROOT, "ui"), os.path.join(tmp, "ui"))
        shutil.copytree(os.path.join(ROOT, "docs"), os.path.join(tmp, "docs"))
        page = os.path.join(tmp, "ui", "index.html")
        html = open(page, encoding="utf8").read().replace("</body>", test + "</body>")
        open(page, "w", encoding="utf8").write(html)
        url = "file:///" + page.replace(os.sep, "/").lstrip("/")
        cmd = [find_browser(), "--headless=new", "--disable-gpu", "--no-sandbox", "--allow-file-access-from-files",
               "--virtual-time-budget=15000", "--dump-dom", url]
        dom = subprocess.run(cmd, capture_output=True, text=True, timeout=180).stdout
        m = re.search(r'<pre id="e2e"[^>]*>(.*?)</pre>', dom, re.S)
        if not m:
            sys.exit("test did not run (no result in the page)")
        text = re.sub(r"<[^>]+>", "", m.group(1))
        print(text)
        sys.exit(0 if "RESULT: ALL PASS" in text else 1)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
