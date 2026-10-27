// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <Utility/SheetController.h>
#include <Panel/NetworkConnectionsManager.h>
#include "ConnectionSheetProtocol.h"

// Sheet for creating or editing an S3 connection (AWS or an S3-compatible service).
// The UI is built in code, there's no NIB behind it.
// "password" carries the secret access key; it's empty for connections that use an AWS CLI profile.
@interface S3ConnectionSheetController : SheetController <ConnectionSheetProtocol, NSTextFieldDelegate>

@property(nonatomic) nc::panel::NetworkConnectionsManager::Connection connection;
@property(nonatomic) std::string password;
@property(nonatomic) bool setupMode;

@end
