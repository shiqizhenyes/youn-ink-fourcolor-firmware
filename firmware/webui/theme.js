(function () {
  var KEY = "inkscreen-web-theme";

  function currentTheme() {
    try {
      return localStorage.getItem(KEY) === "ink" ? "ink" : "luci";
    } catch (e) {
      return "luci";
    }
  }

  function applyTheme(theme) {
    var next = theme === "ink" ? "ink" : "luci";
    document.documentElement.setAttribute("data-theme", next);
    try {
      localStorage.setItem(KEY, next);
    } catch (e) {}
    var buttons = document.querySelectorAll("[data-theme-set]");
    for (var i = 0; i < buttons.length; i++) {
      var btn = buttons[i];
      var on = btn.getAttribute("data-theme-set") === next;
      if (on) btn.classList.add("active");
      else btn.classList.remove("active");
    }
  }

  function currentRoute() {
    var path = (location.pathname || "/").replace(/\/+$/, "") || "/";
    if (path.indexOf("/developer/ble") >= 0 || /ble_web_panel/i.test(path)) return "ble";
    if (path.indexOf("/developer/layout") >= 0 || /layout_adjuster/i.test(path)) return "layout";
    var hash = (location.hash || "").replace(/^#/, "");
    if (hash === "upload") return "photos";
    if (hash) return hash;
    var chrome = document.documentElement.getAttribute("data-chrome");
    return chrome === "captive" ? "wifi" : "photos";
  }

  function routeMatches(spec, route) {
    if (!spec) return false;
    var parts = spec.split("|");
    for (var i = 0; i < parts.length; i++) {
      if (parts[i] === route) return true;
    }
    return false;
  }

  function syncActive() {
    var route = currentRoute();
    var items = document.querySelectorAll("#topmenu [data-route]");
    for (var i = 0; i < items.length; i++) {
      var el = items[i];
      var on = routeMatches(el.getAttribute("data-route"), route);
      var li = el.closest("li");
      if (li) {
        if (on) li.classList.add("active");
        else li.classList.remove("active");
      }
    }
    var tops = document.querySelectorAll("#topmenu > li");
    for (var t = 0; t < tops.length; t++) {
      var topLi = tops[t];
      if (topLi.getAttribute("data-nav") === "theme") {
        topLi.classList.remove("active");
        continue;
      }
      var childActive = topLi.querySelector(".dropdown-menu li.active > [data-route]");
      var selfLink = topLi.children.length ? topLi.children[0] : null;
      var selfOn = selfLink && routeMatches(selfLink.getAttribute("data-route"), route);
      if (childActive || selfOn) topLi.classList.add("active");
      else topLi.classList.remove("active");
    }
  }

  function flattenSoloMenus() {
    var tops = document.querySelectorAll("#topmenu > li.dropdown");
    for (var i = 0; i < tops.length; i++) {
      var li = tops[i];
      var kids = li.querySelectorAll(".dropdown-menu > li");
      var menu = li.querySelector(":scope > a.menu") || li.querySelector("a.menu");
      if (kids.length !== 1) {
        li.classList.remove("solo");
        continue;
      }
      li.classList.add("solo");
      var child = kids[0].querySelector("a");
      if (!menu || !child) continue;
      menu.textContent = child.textContent.replace(/\s+/g, " ").trim();
      var href = child.getAttribute("href");
      if (href) menu.setAttribute("href", href);
      var route = child.getAttribute("data-route");
      if (route) menu.setAttribute("data-route", route);
    }
  }

  function setRouteHash(mode) {
    if (!mode) return;
    var next = "#" + mode;
    if (location.hash === next) return;
    try {
      if (history.replaceState) {
        history.replaceState(null, "", next);
        return;
      }
    } catch (e) {}
    location.hash = mode;
  }

  function scrollContentTop() {
    try {
      window.scrollTo(0, 0);
    } catch (e) {}
    if (document.documentElement) document.documentElement.scrollTop = 0;
    if (document.body) document.body.scrollTop = 0;
  }

  function inPageRoute(anchor) {
    if (!anchor || !anchor.getAttribute) return null;
    if (anchor.getAttribute("data-theme-set")) return null;
    var href = anchor.getAttribute("href") || "";
    if (!href || href === "#") return null;
    if (href.charAt(0) === "#") return href.replace(/^#/, "");
    if (href.indexOf("/#") === 0) {
      var path = (location.pathname || "/").replace(/\/+$/, "") || "/";
      if (path === "/") return href.slice(2);
    }
    return null;
  }

  function notifyHashChange() {
    try {
      window.dispatchEvent(new HashChangeEvent("hashchange"));
    } catch (e) {
      try {
        var evt = document.createEvent("Event");
        evt.initEvent("hashchange", true, true);
        window.dispatchEvent(evt);
      } catch (err) {}
    }
  }

  function followInPageHash(anchor, ev) {
    var route = inPageRoute(anchor);
    if (!route) return false;
    if (ev) ev.preventDefault();
    closeDropdowns(null);
    setRouteHash(route);
    notifyHashChange();
    scrollContentTop();
    syncActive();
    return true;
  }

  function closeDropdowns(except) {
    var open = document.querySelectorAll("#topmenu li.dropdown.open");
    for (var i = 0; i < open.length; i++) {
      if (open[i] !== except) open[i].classList.remove("open");
    }
  }

  function bindNav() {
    var menus = document.querySelectorAll("#topmenu > li.dropdown > a.menu");
    for (var i = 0; i < menus.length; i++) {
      menus[i].addEventListener("click", function (ev) {
        var li = this.parentElement;
        if (!li) return;
        if (li.classList.contains("solo")) {
          if (followInPageHash(this, ev)) return;
          closeDropdowns(null);
          return;
        }
        ev.preventDefault();
        var willOpen = !li.classList.contains("open");
        closeDropdowns(li);
        if (willOpen) li.classList.add("open");
        else li.classList.remove("open");
      });
    }
    var childLinks = document.querySelectorAll("#topmenu .dropdown-menu a[href]");
    for (var j = 0; j < childLinks.length; j++) {
      childLinks[j].addEventListener("click", function (ev) {
        followInPageHash(this, ev);
      });
    }
    document.addEventListener("click", function (ev) {
      if (!ev.target.closest || !ev.target.closest("#topmenu")) closeDropdowns(null);
    });
    window.addEventListener("hashchange", syncActive);
  }

  function bindTheme() {
    applyTheme(currentTheme());
    var buttons = document.querySelectorAll("[data-theme-set]");
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].addEventListener("click", function (ev) {
        ev.preventDefault();
        applyTheme(this.getAttribute("data-theme-set"));
        closeDropdowns(null);
      });
    }
  }

  function bindHelp() {
    var btn = document.getElementById("helpBtn");
    var panel = document.getElementById("helpPanel");
    if (!btn) return;
    if (!panel) {
      btn.style.display = "none";
      return;
    }
    btn.addEventListener("click", function () {
      panel.style.display = panel.style.display === "none" || !panel.style.display ? "block" : "none";
    });
  }

  function initChrome() {
    if (!document.documentElement.getAttribute("data-chrome")) {
      document.documentElement.setAttribute("data-chrome", "lan");
    }
    try {
      if (history.scrollRestoration) history.scrollRestoration = "manual";
    } catch (e) {}
    flattenSoloMenus();
    bindTheme();
    bindNav();
    bindHelp();
    syncActive();
    scrollContentTop();
  }

  applyTheme(currentTheme());
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initChrome);
  } else {
    initChrome();
  }

  window.inkLuciChrome = {
    syncActive: syncActive,
    currentRoute: currentRoute,
    setRouteHash: setRouteHash,
    scrollContentTop: scrollContentTop
  };
})();
