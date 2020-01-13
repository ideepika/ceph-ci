import { Injectable } from '@angular/core';
import { CanActivate, CanActivateChild, Router } from '@angular/router';

import { AuthStorageService } from './auth-storage.service';

@Injectable({
  providedIn: 'root'
})
export class ChangePasswordGuardService implements CanActivate, CanActivateChild {
  constructor(private router: Router, private authStorageService: AuthStorageService) {}

  canActivate() {
    if (
      this.authStorageService.isLoggedIn() &&
      !this.authStorageService.isSSO() &&
      this.authStorageService.getPwdUpdateRequired()
    ) {
      this.router.navigate(['/login-change-password']);
      return false;
    }
    return true;
  }

  canActivateChild(): boolean {
    return this.canActivate();
  }
}
