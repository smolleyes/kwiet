#pragma once

#include <guiddef.h>

// GUIDs owned by Kwiet. Never reuse GUIDs from samples or other projects.
//
// NOTE: CLSID_KwietApo is duplicated as a string in installer/lib/common.ps1
// ($KwietClsid). Keep both in sync.

// COM class of the Kwiet APO, loaded by audiodg.exe.
// {65D564E6-9709-4F5C-85CF-449D92949CFE}
inline constexpr GUID CLSID_KwietApo = {
    0x65D564E6, 0x9709, 0x4F5C, { 0x85, 0xCF, 0x44, 0x9D, 0x92, 0x94, 0x9C, 0xFE } };

// Effect GUID reported through IAudioSystemEffects2::GetEffectsList once noise
// suppression is active (milestone 2+). The passthrough advertises no effect.
// {A67BADF8-A940-432B-80C2-7BDA91971D23}
inline constexpr GUID KWIET_EFFECT_NoiseSuppression = {
    0xA67BADF8, 0xA940, 0x432B, { 0x80, 0xC2, 0x7B, 0xDA, 0x91, 0x97, 0x1D, 0x23 } };

// Audio signal processing modes, redefined locally so translation units do not
// need ksmedia.h and its DEFINE_GUID machinery.
// AUDIO_SIGNALPROCESSINGMODE_DEFAULT {C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}
inline constexpr GUID KwietModeDefault = {
    0xC18E2F7E, 0x933D, 0x4965, { 0xB7, 0xD1, 0x1E, 0xEF, 0x22, 0x8D, 0x2A, 0xF3 } };
// AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS {98951333-B9CD-48B1-A0A3-FF40682D73F7}
inline constexpr GUID KwietModeCommunications = {
    0x98951333, 0xB9CD, 0x48B1, { 0xA0, 0xA3, 0xFF, 0x40, 0x68, 0x2D, 0x73, 0xF7 } };
