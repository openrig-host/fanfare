#pragma once

#include <JuceHeader.h>
#include <vector>

// Theme.h - role-based color system.
//
//   - Studio (default): Nota-style flat neutral greys, single orange accent
//     (#FFA02F), FOH green / IEM blue. Rendered flat by BoutiqueLookAndFeel.
//
// Shared source of truth with the Tablet HUD (`Source/Web/static/style.css`):
//   - Front Panel Dark: charcoal metal (#1B1F22), hairline borders, engraved
//     labels, single teal decorative accent (#3FD0C9), gold FOH (#D9A441),
//     cyan IEM (#53B9D6), guarded red PANIC (#D63B32).
//   - Front Panel Bright: warm paper (#ECEBE5) + deep teal (#0E7C6B) for
//     sunlit outdoor stages.
//   - High Contrast: maximum contrast outdoor/accessibility skin.

struct Theme {
    juce::String id;
    juce::String name;
    bool flat = true;
    bool dark = true;

    // ===== CORE SURFACES =====
    juce::Colour background;     // app canvas (#131619 dark / #DDDAD2 bright)
    juce::Colour panel;          // main rack panel face (#1B1F22 / #ECEBE5)
    juce::Colour panelAlt;       // alternating / raised panel (#23282D / #E3E1DA)
    juce::Colour raised;         // active/raised plate (#2A3036 / #F7F6F2)
    juce::Colour border;         // standard hairline border
    juce::Colour borderStrong;   // emphasized hairline border

    // ===== TEXT =====
    juce::Colour text;
    juce::Colour textDim;
    juce::Colour textFaint;
    juce::Colour textOnAccent;

    // ===== ACCENT + DUAL BUS =====
    juce::Colour accent;         // panel teal - the single decorative accent
    juce::Colour accentDim;
    juce::Colour foh;            // FOH bus = hardware gold
    juce::Colour iem;            // IEM bus = hardware cyan

    // ===== STATUS =====
    juce::Colour ok;             // sage green (assigned / healthy)
    juce::Colour warn;           // brass amber (caution / preloading)
    juce::Colour danger;         // brick red (mute / error)
    juce::Colour panic;          // guarded PANIC red cap
    juce::Colour active;         // active highlight
    juce::Colour bypassed;       // bypassed / unlit LED segment

    // ===== METERS =====
    juce::Colour meterLow;
    juce::Colour meterMid;
    juce::Colour meterPeak;

    // ===== SLOT CATEGORIES (3px top stage-tape stripes) =====
    juce::Colour catMonitor;
    juce::Colour catKeyboard;
    juce::Colour catAccordion;
    juce::Colour catReturn;
    juce::Colour catDefault;

    // ===== MIDI MONITOR =====
    juce::Colour midiNote;
    juce::Colour midiCC;
    juce::Colour midiPC;
    juce::Colour midiArm;
    juce::Colour midiOther;

    // ===== MISC / HARDWARE RENDERING =====
    juce::Colour scrim;
    juce::Colour knobFace;
    juce::Colour trackGroove;
    juce::Colour knobThumb;

    // ===== FRONT PANEL HARDWARE ROLES =====
    juce::Colour canvas;
    juce::Colour sunken;         // recessed LCD / meter / slot well
    juce::Colour hairline;       // crisp 1px panel seam
    juce::Colour hairlineStrong;
    juce::Colour engrave;        // silk-screened uppercase panel labels
    juce::Colour faceTop;        // hardware switch top gradient
    juce::Colour faceBot;        // hardware switch bottom gradient
    juce::Colour pressTop;       // pressed switch top gradient
    juce::Colour pressBot;       // pressed switch bottom gradient
    juce::Colour bevelHi;        // 1px top bevel highlight
    juce::Colour vuFace;         // cream analog VU faceplate
    juce::Colour faderCapFoh;
    juce::Colour faderCapIem;
    juce::Colour helpFaceTop;    // warm amber HAAALP! cap top
    juce::Colour helpFaceBot;    // warm amber HAAALP! cap bottom
    juce::Colour helpText;       // dark engraved ink on HAAALP! cap

    enum class Role {
        background, panel, panelAlt, raised, border, borderStrong,
        text, textDim, textFaint, textOnAccent,
        accent, accentDim, foh, iem,
        ok, warn, danger, panic, active, bypassed,
        meterLow, meterMid, meterPeak,
        catMonitor, catKeyboard, catAccordion, catReturn, catDefault,
        midiNote, midiCC, midiPC, midiArm, midiOther,
        scrim, knobFace, trackGroove, knobThumb,
        canvas, sunken, hairline, hairlineStrong, engrave,
        faceTop, faceBot, pressTop, pressBot, bevelHi,
        vuFace, faderCapFoh, faderCapIem,
        helpFaceTop, helpFaceBot, helpText
    };

    juce::Colour get(Role r) const {
        switch (r) {
            case Role::background:      return background;
            case Role::panel:           return panel;
            case Role::panelAlt:        return panelAlt;
            case Role::raised:          return raised;
            case Role::border:          return border;
            case Role::borderStrong:    return borderStrong;
            case Role::text:            return text;
            case Role::textDim:         return textDim;
            case Role::textFaint:       return textFaint;
            case Role::textOnAccent:    return textOnAccent;
            case Role::accent:          return accent;
            case Role::accentDim:       return accentDim;
            case Role::foh:             return foh;
            case Role::iem:             return iem;
            case Role::ok:              return ok;
            case Role::warn:            return warn;
            case Role::danger:          return danger;
            case Role::panic:           return panic;
            case Role::active:          return active;
            case Role::bypassed:        return bypassed;
            case Role::meterLow:        return meterLow;
            case Role::meterMid:        return meterMid;
            case Role::meterPeak:       return meterPeak;
            case Role::catMonitor:      return catMonitor;
            case Role::catKeyboard:     return catKeyboard;
            case Role::catAccordion:    return catAccordion;
            case Role::catReturn:       return catReturn;
            case Role::catDefault:      return catDefault;
            case Role::midiNote:        return midiNote;
            case Role::midiCC:          return midiCC;
            case Role::midiPC:          return midiPC;
            case Role::midiArm:         return midiArm;
            case Role::midiOther:       return midiOther;
            case Role::scrim:           return scrim;
            case Role::knobFace:        return knobFace;
            case Role::trackGroove:     return trackGroove;
            case Role::knobThumb:       return knobThumb;
            case Role::canvas:          return canvas;
            case Role::sunken:          return sunken;
            case Role::hairline:        return hairline;
            case Role::hairlineStrong:  return hairlineStrong;
            case Role::engrave:         return engrave;
            case Role::faceTop:         return faceTop;
            case Role::faceBot:         return faceBot;
            case Role::pressTop:        return pressTop;
            case Role::pressBot:        return pressBot;
            case Role::bevelHi:         return bevelHi;
            case Role::vuFace:          return vuFace;
            case Role::faderCapFoh:     return faderCapFoh;
            case Role::faderCapIem:     return faderCapIem;
            case Role::helpFaceTop:     return helpFaceTop;
            case Role::helpFaceBot:     return helpFaceBot;
            case Role::helpText:        return helpText;
        }
        return juce::Colours::magenta;
    }

    // Studio style = Nota-like flat rendering: no bevels/gradients/engraving,
    // sentence-case labels, 1px hairlines, single orange accent. The Front
    // Panel themes keep the hardware look. BoutiqueLookAndFeel branches on this.
    bool isStudio() const { return id.startsWith("studio"); }

    static const std::vector<Theme>& all() {
        static const std::vector<Theme> themes = {
            // ---------- 0. Studio (default; Nota-style calm greys + orange) ----------
            // Accent orange = active/selected/armed only. FOH green / IEM blue
            // stay clear of the accent and of danger red.
            {"studio", "Studio", true, true,
             /*background*/     juce::Colour(0xFF1C1D1F),
             /*panel*/          juce::Colour(0xFF232427),
             /*panelAlt*/       juce::Colour(0xFF2A2B2E),
             /*raised*/         juce::Colour(0xFF313236),
             /*border*/         juce::Colour(0xFF38393D),
             /*borderStrong*/   juce::Colour(0xFF4A4B50),
             /*text*/           juce::Colour(0xFFE6E6E6),
             /*textDim*/        juce::Colour(0xFFA0A1A5),
             /*textFaint*/      juce::Colour(0xFF6E6F74),
             /*textOnAccent*/   juce::Colour(0xFF1C1409),
             /*accent*/         juce::Colour(0xFFFFA02F),
             /*accentDim*/      juce::Colour(0xFFB8701F),
             /*foh*/            juce::Colour(0xFF5FC08A),
             /*iem*/            juce::Colour(0xFF5AA9F0),
             /*ok*/             juce::Colour(0xFF5FC08A),
             /*warn*/           juce::Colour(0xFFE5C07B),
             /*danger*/         juce::Colour(0xFFE5534B),
             /*panic*/          juce::Colour(0xFFE0443B),
             /*active*/         juce::Colour(0xFFFFA02F),
             /*bypassed*/       juce::Colour(0xFF3A3B3F),
             /*meterLow*/       juce::Colour(0xFF5FC08A),
             /*meterMid*/       juce::Colour(0xFFE5C07B),
             /*meterPeak*/      juce::Colour(0xFFE5534B),
             /*catMonitor*/     juce::Colour(0xFFC9915A),
             /*catKeyboard*/    juce::Colour(0xFF6F9FC8),
             /*catAccordion*/   juce::Colour(0xFFC0645E),
             /*catReturn*/      juce::Colour(0xFF7C84C4),
             /*catDefault*/     juce::Colour(0xFF4A4B50),
             /*midiNote*/       juce::Colour(0xFF5FC08A),
             /*midiCC*/         juce::Colour(0xFF5AA9F0),
             /*midiPC*/         juce::Colour(0xFFB08AE0),
             /*midiArm*/        juce::Colour(0xFFFFA02F),
             /*midiOther*/      juce::Colour(0xFFE08D5A),
             /*scrim*/          juce::Colour(0xD9111214),
             /*knobFace*/       juce::Colour(0xFF313236),
             /*trackGroove*/    juce::Colour(0xFF151618),
             /*knobThumb*/      juce::Colour(0xFFE6E6E6),
             /*canvas*/         juce::Colour(0xFF1C1D1F),
             /*sunken*/         juce::Colour(0xFF18191B),
             /*hairline*/       juce::Colour(0x1FFFFFFF),
             /*hairlineStrong*/ juce::Colour(0x33FFFFFF),
             /*engrave*/        juce::Colour(0xFFA0A1A5),
             /*faceTop*/        juce::Colour(0xFF2E2F33),
             /*faceBot*/        juce::Colour(0xFF2E2F33),
             /*pressTop*/       juce::Colour(0xFF26272A),
             /*pressBot*/       juce::Colour(0xFF26272A),
             /*bevelHi*/        juce::Colour(0x00FFFFFF),
             /*vuFace*/         juce::Colour(0xFFF3EFE6),
             /*faderCapFoh*/    juce::Colour(0xFF5FC08A),
             /*faderCapIem*/    juce::Colour(0xFF5AA9F0),
             /*helpFaceTop*/    juce::Colour(0xFF2E2F33),
             /*helpFaceBot*/    juce::Colour(0xFF2E2F33),
             /*helpText*/       juce::Colour(0xFFE6E6E6)},

            // ---------- 1. Front Panel Dark ----------
            {"frontPanelDark", "Front Panel Dark", true, true,
             /*background*/     juce::Colour(0xFF131619),
             /*panel*/          juce::Colour(0xFF1B1F22),
             /*panelAlt*/       juce::Colour(0xFF23282D),
             /*raised*/         juce::Colour(0xFF2A3036),
             /*border*/         juce::Colour(0xFF333A41),
             /*borderStrong*/   juce::Colour(0xFF49535D),
             /*text*/           juce::Colour(0xFFE9E7E2),
             /*textDim*/        juce::Colour(0xFF969DA3),
             /*textFaint*/      juce::Colour(0xFF687077),
             /*textOnAccent*/   juce::Colour(0xFF0E1515),
             /*accent*/         juce::Colour(0xFF3FD0C9),
             /*accentDim*/      juce::Colour(0xFF2A8F8A),
             /*foh*/            juce::Colour(0xFFD9A441),
             /*iem*/            juce::Colour(0xFF53B9D6),
             /*ok*/             juce::Colour(0xFF5CB96E),
             /*warn*/           juce::Colour(0xFFD9A441),
             /*danger*/         juce::Colour(0xFFC94F4A),
             /*panic*/          juce::Colour(0xFFD63B32),
             /*active*/         juce::Colour(0xFF3FD0C9),
             /*bypassed*/       juce::Colour(0xFF3A4046),
             /*meterLow*/       juce::Colour(0xFF4CAF5E),
             /*meterMid*/       juce::Colour(0xFFD9A441),
             /*meterPeak*/      juce::Colour(0xFFE0524A),
             /*catMonitor*/     juce::Colour(0xFFC88A32),
             /*catKeyboard*/    juce::Colour(0xFF3D8A94),
             /*catAccordion*/   juce::Colour(0xFFA84840),
             /*catReturn*/      juce::Colour(0xFF4A78A8),
             /*catDefault*/     juce::Colour(0xFF4A525A),
             /*midiNote*/       juce::Colour(0xFF5CB96E),
             /*midiCC*/         juce::Colour(0xFF53B9D6),
             /*midiPC*/         juce::Colour(0xFF9A86D9),
             /*midiArm*/        juce::Colour(0xFFD9A441),
             /*midiOther*/      juce::Colour(0xFFE08A3C),
             /*scrim*/          juce::Colour(0xD90B0E10),
             /*knobFace*/       juce::Colour(0xFF2E343B),
             /*trackGroove*/    juce::Colour(0xFF0E1113),
             /*knobThumb*/      juce::Colour(0xFFE8E4DA),
             /*canvas*/         juce::Colour(0xFF131619),
             /*sunken*/         juce::Colour(0xFF0E1113),
             /*hairline*/       juce::Colour(0x22FFFFFF),
             /*hairlineStrong*/ juce::Colour(0x3DFFFFFF),
             /*engrave*/        juce::Colour(0x70FFFFFF),
             /*faceTop*/        juce::Colour(0xFF2A3036),
             /*faceBot*/        juce::Colour(0xFF1E2327),
             /*pressTop*/       juce::Colour(0xFF171B1E),
             /*pressBot*/       juce::Colour(0xFF20262B),
             /*bevelHi*/        juce::Colour(0x1CFFFFFF),
             /*vuFace*/         juce::Colour(0xFFFAF6EB),
             /*faderCapFoh*/    juce::Colour(0xFFD9A441),
             /*faderCapIem*/    juce::Colour(0xFF53B9D6),
             /*helpFaceTop*/    juce::Colour(0xFFE8A52E),
             /*helpFaceBot*/    juce::Colour(0xFFC9861A),
             /*helpText*/       juce::Colour(0xFF1A1205)},

            // ---------- 2. Front Panel Bright (warm paper + teal) ----------
            {"frontPanelBright", "Front Panel Bright", true, false,
             /*background*/     juce::Colour(0xFFDDDAD2),
             /*panel*/          juce::Colour(0xFFECEBE5),
             /*panelAlt*/       juce::Colour(0xFFE4E2DA),
             /*raised*/         juce::Colour(0xFFFAF9F5),
             /*border*/         juce::Colour(0xFFB5B1A7),
             /*borderStrong*/   juce::Colour(0xFF88847B),
             /*text*/           juce::Colour(0xFF14181C),
             /*textDim*/        juce::Colour(0xFF343B41),
             /*textFaint*/      juce::Colour(0xFF535B62),
             /*textOnAccent*/   juce::Colour(0xFFFFFFFF),
             /*accent*/         juce::Colour(0xFF096657),
             /*accentDim*/      juce::Colour(0xFF074F43),
             /*foh*/            juce::Colour(0xFF7D5206),
             /*iem*/            juce::Colour(0xFF0D5B7A),
             /*ok*/             juce::Colour(0xFF1D6E33),
             /*warn*/           juce::Colour(0xFF8A5706),
             /*danger*/         juce::Colour(0xFF9E2B25),
             /*panic*/          juce::Colour(0xFFB82820),
             /*active*/         juce::Colour(0xFF096657),
             /*bypassed*/       juce::Colour(0xFFBEB9AE),
             /*meterLow*/       juce::Colour(0xFF287D3E),
             /*meterMid*/       juce::Colour(0xFFB87F14),
             /*meterPeak*/      juce::Colour(0xFFC42D24),
             /*catMonitor*/     juce::Colour(0xFFA45A17),
             /*catKeyboard*/    juce::Colour(0xFF0C7362),
             /*catAccordion*/   juce::Colour(0xFFA83A34),
             /*catReturn*/      juce::Colour(0xFF245F8F),
             /*catDefault*/     juce::Colour(0xFF6E757B),
             /*midiNote*/       juce::Colour(0xFF1D6E33),
             /*midiCC*/         juce::Colour(0xFF0D5B7A),
             /*midiPC*/         juce::Colour(0xFF524094),
             /*midiArm*/        juce::Colour(0xFF8A5706),
             /*midiOther*/      juce::Colour(0xFF984F10),
             /*scrim*/          juce::Colour(0xB8202428),
             /*knobFace*/       juce::Colour(0xFFDAD7CE),
             /*trackGroove*/    juce::Colour(0xFFC9C6BD),
             /*knobThumb*/      juce::Colour(0xFF14181C),
             /*canvas*/         juce::Colour(0xFFDDDAD2),
             /*sunken*/         juce::Colour(0xFFE0DDD4),
             /*hairline*/       juce::Colour(0x4214181C),
             /*hairlineStrong*/ juce::Colour(0x7814181C),
             /*engrave*/        juce::Colour(0xCA14181C),
             /*faceTop*/        juce::Colour(0xFFFCFBF8),
             /*faceBot*/        juce::Colour(0xFFE5E3DC),
             /*pressTop*/       juce::Colour(0xFFD6D3CB),
             /*pressBot*/       juce::Colour(0xFFECEBE5),
             /*bevelHi*/        juce::Colour(0xE6FFFFFF),
             /*vuFace*/         juce::Colour(0xFFFAF6EB),
             /*faderCapFoh*/    juce::Colour(0xFFD9A441),
             /*faderCapIem*/    juce::Colour(0xFF53B9D6),
             /*helpFaceTop*/    juce::Colour(0xFFE8A52E),
             /*helpFaceBot*/    juce::Colour(0xFFC9861A),
             /*helpText*/       juce::Colour(0xFF1A1205)},

            // ---------- 3. High Contrast (accessibility / outdoor) ----------
            {"highContrast", "High Contrast", true, true,
             /*background*/     juce::Colour(0xFF000000),
             /*panel*/          juce::Colour(0xFF0A0C0E),
             /*panelAlt*/       juce::Colour(0xFF14181C),
             /*raised*/         juce::Colour(0xFF1E242A),
             /*border*/         juce::Colour(0xFFFFFFFF),
             /*borderStrong*/   juce::Colour(0xFFFFFFFF),
             /*text*/           juce::Colour(0xFFFFFFFF),
             /*textDim*/        juce::Colour(0xFFD6D6D6),
             /*textFaint*/      juce::Colour(0xFF9E9E9E),
             /*textOnAccent*/   juce::Colour(0xFF000000),
             /*accent*/         juce::Colour(0xFF3FF5EB),
             /*accentDim*/      juce::Colour(0xFF24B5AD),
             /*foh*/            juce::Colour(0xFFFFCA28),
             /*iem*/            juce::Colour(0xFF29D4FF),
             /*ok*/             juce::Colour(0xFF4CFF72),
             /*warn*/           juce::Colour(0xFFFFCA28),
             /*danger*/         juce::Colour(0xFFFF4040),
             /*panic*/          juce::Colour(0xFFFF1F1F),
             /*active*/         juce::Colour(0xFF3FF5EB),
             /*bypassed*/       juce::Colour(0xFF555555),
             /*meterLow*/       juce::Colour(0xFF4CFF72),
             /*meterMid*/       juce::Colour(0xFFFFCA28),
             /*meterPeak*/      juce::Colour(0xFFFF3030),
             /*catMonitor*/     juce::Colour(0xFFFF9800),
             /*catKeyboard*/    juce::Colour(0xFF3FF5EB),
             /*catAccordion*/   juce::Colour(0xFFFF4040),
             /*catReturn*/      juce::Colour(0xFF29D4FF),
             /*catDefault*/     juce::Colour(0xFF9E9E9E),
             /*midiNote*/       juce::Colour(0xFF4CFF72),
             /*midiCC*/         juce::Colour(0xFF29D4FF),
             /*midiPC*/         juce::Colour(0xFFD080FF),
             /*midiArm*/        juce::Colour(0xFFFFCA28),
             /*midiOther*/      juce::Colour(0xFFFF9800),
             /*scrim*/          juce::Colour(0xEB000000),
             /*knobFace*/       juce::Colour(0xFF242A30),
             /*trackGroove*/    juce::Colour(0xFF000000),
             /*knobThumb*/      juce::Colour(0xFFFFFFFF),
             /*canvas*/         juce::Colour(0xFF000000),
             /*sunken*/         juce::Colour(0xFF040506),
             /*hairline*/       juce::Colour(0xB0FFFFFF),
             /*hairlineStrong*/ juce::Colour(0xFFFFFFFF),
             /*engrave*/        juce::Colour(0xE0FFFFFF),
             /*faceTop*/        juce::Colour(0xFF242A30),
             /*faceBot*/        juce::Colour(0xFF14181C),
             /*pressTop*/       juce::Colour(0xFF0A0C0E),
             /*pressBot*/       juce::Colour(0xFF1A2026),
             /*bevelHi*/        juce::Colour(0x80FFFFFF),
             /*vuFace*/         juce::Colour(0xFFFFFBF0),
             /*faderCapFoh*/    juce::Colour(0xFFFFCA28),
             /*faderCapIem*/    juce::Colour(0xFF29D4FF),
             /*helpFaceTop*/    juce::Colour(0xFFFFCA28),
             /*helpFaceBot*/    juce::Colour(0xFFE0A800),
             /*helpText*/       juce::Colour(0xFF000000)},
        };
        return themes;
    }

    static juce::String normalizeThemeId(const juce::String& rawId) {
        if (rawId == "stageBlack" || rawId == "amberConsole" || rawId == "sapphire")
            return "frontPanelDark";
        if (rawId == "daylight")
            return "frontPanelBright";
        return rawId;
    }

    static const Theme* findById(const juce::String& targetId) {
        juce::String norm = normalizeThemeId(targetId);
        for (const auto& t : all())
            if (t.id == norm)
                return &t;
        return nullptr;
    }
};
