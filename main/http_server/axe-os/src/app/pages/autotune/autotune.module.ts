import { NgModule } from '@angular/core';
import { CommonModule } from '@angular/common';
import { AutotuneComponent } from './autotune.component';
import { 
  NbCardModule, 
  NbButtonModule, 
  NbIconModule, 
  NbBadgeModule,
  NbTooltipModule,
  NbInputModule
} from '@nebular/theme';
import { FormsModule } from '@angular/forms';
import { SystemService } from '../../services/system.service';
import { TranslateModule } from '@ngx-translate/core';

@NgModule({
  declarations: [
    AutotuneComponent,
  ],
  imports: [
    CommonModule,
    NbCardModule,
    NbButtonModule,
    NbIconModule,
    NbBadgeModule,
    NbTooltipModule,
    NbInputModule,
    TranslateModule,
    FormsModule,
  ],
  providers: [
    SystemService
  ],
  exports: [
    AutotuneComponent
  ]
})
export class AutotuneModule {}
