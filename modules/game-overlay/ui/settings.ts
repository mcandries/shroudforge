import type { TranslationKey } from '../../ui-resources/i18n'
// hideKey is not listed: show-module-settings renders it with KeyComboInput.
export const gameOverlaySettingsFields: Array<
  [string, string, TranslationKey, string[]?]
> = [
  ['gameOverlay', 'scale', 'settings.modules.field.overlayScale'],
  ['gameOverlay', 'opacity', 'settings.modules.field.overlayOpacity'],
]
