import { Injectable } from '@angular/core';

import { I18n } from '@ngx-translate/i18n-polyfill';
import { forkJoin, Observable } from 'rxjs';
import { map } from 'rxjs/operators';

import { SettingsService } from '../api/settings.service';

@Injectable({
  providedIn: 'root'
})
export class PasswordPolicyService {
  constructor(private i18n: I18n, private settingsService: SettingsService) {}

  getHelpText(): Observable<string> {
    const observables = {
      enabled: this.settingsService.getValue('PWD_POLICY_ENABLED'),
      min_length: this.settingsService.getValue('PWD_POLICY_MIN_LENGTH'),
      chk_length: this.settingsService.getValue('PWD_POLICY_CHECK_LENGTH_ENABLED'),
      chk_oldpwd: this.settingsService.getValue('PWD_POLICY_CHECK_OLDPWD_ENABLED'),
      chk_username: this.settingsService.getValue('PWD_POLICY_CHECK_USERNAME_ENABLED'),
      chk_exclusion_list: this.settingsService.getValue('PWD_POLICY_CHECK_EXCLUSION_LIST_ENABLED'),
      chk_repetitive: this.settingsService.getValue('PWD_POLICY_CHECK_REPETITIVE_CHARS_ENABLED'),
      chk_sequential: this.settingsService.getValue('PWD_POLICY_CHECK_SEQUENTIAL_CHARS_ENABLED'),
      chk_complexity: this.settingsService.getValue('PWD_POLICY_CHECK_COMPLEXITY_ENABLED')
    };
    return forkJoin(observables).pipe(
      map((resp: Object) => {
        const helpText: string[] = [];
        if (resp['enabled']) {
          helpText.push(this.i18n('Required rules for passwords:'));
          if (resp['chk_length']) {
            helpText.push(
              '- ' +
                this.i18n('Must contain at least {{length}} characters', {
                  length: resp['min_length']
                })
            );
          }
          if (resp['chk_oldpwd']) {
            helpText.push('- ' + this.i18n('Must not be the same as the previous one'));
          }
          if (resp['chk_username']) {
            helpText.push('- ' + this.i18n('Cannot contain the username'));
          }
          if (resp['chk_exclusion_list']) {
            helpText.push('- ' + this.i18n('Cannot contain any configured keyword'));
          }
          if (resp['chk_repetitive']) {
            helpText.push('- ' + this.i18n('Cannot contain any repetitive characters e.g. "aaa"'));
          }
          if (resp['chk_sequential']) {
            helpText.push('- ' + this.i18n('Cannot contain any sequential characters e.g. "abc"'));
          }
          if (resp['chk_complexity']) {
            helpText.push(
              '- ' +
                this.i18n(
                  'Must consist of characters from the following groups:\n' +
                    '  * Alphabetic a-z, A-Z\n' +
                    '  * Numbers 0-9\n' +
                    '  * Special chars: !"#$%& \'()*+,-./:;<=>?@[\\]^_`{{|}}~\n' +
                    '  * Any other characters (signs)'
                )
            );
          }
        }
        return helpText.join('\n');
      })
    );
  }

  /**
   * Helper function to map password policy credits to a CSS class.
   * @param credits The password policy credits.
   * @return The name of the CSS class.
   */
  mapCreditsToCssClass(credits: number): string {
    let result = 'very-strong';
    if (credits < 10) {
      result = 'too-weak';
    } else if (credits < 15) {
      result = 'weak';
    } else if (credits < 20) {
      result = 'ok';
    } else if (credits < 25) {
      result = 'strong';
    }
    return result;
  }
}
