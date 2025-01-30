import { Component, OnInit, ViewChild } from '@angular/core';
import { catchError, switchMap } from 'rxjs/operators';
import { BehaviorSubject, Observable, of } from 'rxjs';

import _ from 'lodash';

import { ActionLabelsI18n } from '~/app/shared/constants/app.constants';
import { TableComponent } from '~/app/shared/datatable/table/table.component';
import { CdTableAction } from '~/app/shared/models/cd-table-action';
import { CdTableColumn } from '~/app/shared/models/cd-table-column';
import { CdTableFetchDataContext } from '~/app/shared/models/cd-table-fetch-data-context';
import { ListWithDetails } from '~/app/shared/classes/list-with-details.class';
import { Permission } from '~/app/shared/models/permissions';

import { AuthStorageService } from '~/app/shared/services/auth-storage.service';
import { SmbService } from '~/app/shared/api/smb.service';

import { Icons } from '~/app/shared/enum/icons.enum';
import { CdTableSelection } from '~/app/shared/models/cd-table-selection';
import { URLBuilderService } from '~/app/shared/services/url-builder.service';
import { SMBCluster } from '../smb.model';
import { NgbModalRef } from '@ng-bootstrap/ng-bootstrap';
import { ModalCdsService } from '~/app/shared/services/modal-cds.service';
import { CriticalConfirmationModalComponent } from '~/app/shared/components/critical-confirmation-modal/critical-confirmation-modal.component';
import { TaskWrapperService } from '~/app/shared/services/task-wrapper.service';
import { FinishedTask } from '~/app/shared/models/finished-task';

const BASE_URL = 'cephfs/smb';
@Component({
  selector: 'cd-smb-cluster-list',
  templateUrl: './smb-cluster-list.component.html',
  styleUrls: ['./smb-cluster-list.component.scss'],
  providers: [{ provide: URLBuilderService, useValue: new URLBuilderService(BASE_URL) }]
})
export class SmbClusterListComponent extends ListWithDetails implements OnInit {
  @ViewChild('table', { static: true })
  table: TableComponent;
  columns: CdTableColumn[];
  permission: Permission;
  tableActions: CdTableAction[];
  context: CdTableFetchDataContext;
  selection = new CdTableSelection();
  smbClusters$: Observable<SMBCluster[]>;
  subject$ = new BehaviorSubject<SMBCluster[]>([]);
  modalRef: NgbModalRef;

  constructor(
    private authStorageService: AuthStorageService,
    public actionLabels: ActionLabelsI18n,
    private smbService: SmbService,
    private modalService: ModalCdsService,
    private taskWrapper: TaskWrapperService,
    private urlBuilder: URLBuilderService
  ) {
    super();
    this.permission = this.authStorageService.getPermissions().smb;
  }

  ngOnInit() {
    this.columns = [
      {
        name: $localize`Name`,
        prop: 'cluster_id',
        flexGrow: 2
      },
      {
        name: $localize`Authentication Mode`,
        prop: 'auth_mode',
        flexGrow: 2
      }
    ];
    this.tableActions = [
      {
        name: `${this.actionLabels.CREATE}`,
        permission: 'create',
        icon: Icons.add,
        routerLink: () => this.urlBuilder.getCreate(),

        canBePrimary: (selection: CdTableSelection) => !selection.hasSingleSelection
      },
      {
        permission: 'delete',
        icon: Icons.destroy,
        click: () => this.removeSMBClusterModal(),
        name: this.actionLabels.REMOVE
      }
    ];

    this.smbClusters$ = this.subject$.pipe(
      switchMap(() =>
        this.smbService.listClusters()?.pipe(
          catchError(() => {
            this.context.error();
            return of(null);
          })
        )
      )
    );
  }

  loadSMBCluster() {
    this.subject$.next([]);
  }

  updateSelection(selection: CdTableSelection) {
    this.selection = selection;
  }

  removeSMBClusterModal() {
    const cluster_id = this.selection.first().cluster_id;

    this.modalService.show(CriticalConfirmationModalComponent, {
      itemDescription: $localize`Cluster`,
      itemNames: [cluster_id],
      actionDescription: $localize`remove`,
      submitActionObservable: () =>
        this.taskWrapper.wrapTaskAroundCall({
          task: new FinishedTask('smb/cluster/remove', {
            cluster_id: cluster_id
          }),
          call: this.smbService.removeCluster(cluster_id)
        })
    });
  }
}
