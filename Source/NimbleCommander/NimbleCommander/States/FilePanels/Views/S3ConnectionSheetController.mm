// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "S3ConnectionSheetController.h"
#include <VFS/NetS3.h>
#include <Utility/StringExtras.h>
#include <vector>

using nc::panel::NetworkConnectionsManager;

namespace {

enum class S3Service : NSInteger {
    AWS = 0,
    Custom = 1
};

constexpr NSInteger g_ManualKeysTag = -1; // tags >= 0 are indices into the loaded AWS profiles

NSString *S3SafeNSString(const std::string &_s)
{
    NSString *const str = [NSString stringWithUTF8StdString:_s];
    return str ? str : @"";
}

std::string S3TrimmedStdString(NSTextField *_field)
{
    NSString *const v = [_field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    return v.UTF8String ? v.UTF8String : "";
}

} // namespace

@implementation S3ConnectionSheetController {
    std::optional<NetworkConnectionsManager::Connection> m_Original;
    std::optional<std::string> m_Password;
    NetworkConnectionsManager::S3 m_Result;
    std::vector<nc::vfs::s3::AwsProfile> m_Profiles;

    NSTextField *m_TitleField;
    NSPopUpButton *m_ServicePopup;
    NSTextField *m_EndpointField;
    NSTextField *m_RegionField;
    NSTextField *m_BucketField;
    NSPopUpButton *m_CredentialsPopup;
    NSTextField *m_AccessKeyField;
    NSSecureTextField *m_SecretKeyField;
    NSButton *m_PathStyleCheckbox;
    NSButton *m_ConnectButton;
    NSTextField *m_HintLabel;
}
@synthesize setupMode;

- (instancetype)init
{
    NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 360)
                                                         styleMask:NSWindowStyleMaskTitled
                                                           backing:NSBackingStoreBuffered
                                                             defer:true];
    self = [super initWithWindow:window];
    if( self ) {
        m_Profiles = nc::vfs::s3::LoadAwsProfiles();
        [self buildInterface];
    }
    return self;
}

//==================================================================================================
// Interface construction
//==================================================================================================

- (NSTextField *)makeLabel:(NSString *)_text
{
    NSTextField *const label = [NSTextField labelWithString:_text];
    label.alignment = NSTextAlignmentRight;
    return label;
}

- (NSTextField *)makeField:(NSString *)_placeholder
{
    NSTextField *const field = [[NSTextField alloc] init];
    field.placeholderString = _placeholder;
    field.delegate = self;
    field.usesSingleLineMode = true;
    field.cell.scrollable = true;
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:320].active = true;
    return field;
}

- (void)buildInterface
{
    m_TitleField = [self makeField:NSLocalizedString(@"optional", "")];

    m_ServicePopup = [[NSPopUpButton alloc] init];
    [m_ServicePopup addItemWithTitle:NSLocalizedString(@"Amazon S3", "")];
    m_ServicePopup.lastItem.tag = static_cast<NSInteger>(S3Service::AWS);
    [m_ServicePopup addItemWithTitle:NSLocalizedString(@"S3-compatible (MinIO, R2, Wasabi, ...)", "")];
    m_ServicePopup.lastItem.tag = static_cast<NSInteger>(S3Service::Custom);
    m_ServicePopup.target = self;
    m_ServicePopup.action = @selector(onServiceChanged:);

    m_EndpointField = [self makeField:@"https://minio.example.com:9000"];
    m_RegionField = [self makeField:@"us-east-1"];
    m_BucketField = [self makeField:NSLocalizedString(@"optional, empty lists all buckets", "")];

    m_CredentialsPopup = [[NSPopUpButton alloc] init];
    [m_CredentialsPopup addItemWithTitle:NSLocalizedString(@"Access keys entered below", "")];
    m_CredentialsPopup.lastItem.tag = g_ManualKeysTag;
    if( !m_Profiles.empty() ) {
        [m_CredentialsPopup.menu addItem:NSMenuItem.separatorItem];
        for( size_t i = 0; i < m_Profiles.size(); ++i ) {
            NSString *const title = [NSString
                stringWithFormat:NSLocalizedString(@"AWS profile “%@” (~/.aws)", ""), S3SafeNSString(m_Profiles[i].name)];
            [m_CredentialsPopup addItemWithTitle:title];
            m_CredentialsPopup.lastItem.tag = static_cast<NSInteger>(i);
        }
    }
    m_CredentialsPopup.target = self;
    m_CredentialsPopup.action = @selector(onCredentialsChanged:);

    m_AccessKeyField = [self makeField:@"AKIA..."];
    m_SecretKeyField = [[NSSecureTextField alloc] init];
    m_SecretKeyField.delegate = self;
    m_SecretKeyField.placeholderString = NSLocalizedString(@"stored in the Keychain", "");

    m_PathStyleCheckbox = [NSButton checkboxWithTitle:NSLocalizedString(@"Use path-style addressing", "")
                                               target:nil
                                               action:nil];

    m_HintLabel = [NSTextField wrappingLabelWithString:@""];
    m_HintLabel.textColor = NSColor.secondaryLabelColor;
    m_HintLabel.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [m_HintLabel.widthAnchor constraintLessThanOrEqualToConstant:320].active = true;

    NSGridView *const grid = [NSGridView gridViewWithViews:@[
        @[[self makeLabel:NSLocalizedString(@"Title:", "")], m_TitleField],
        @[[self makeLabel:NSLocalizedString(@"Service:", "")], m_ServicePopup],
        @[[self makeLabel:NSLocalizedString(@"Endpoint:", "")], m_EndpointField],
        @[[self makeLabel:NSLocalizedString(@"Region:", "")], m_RegionField],
        @[[self makeLabel:NSLocalizedString(@"Bucket:", "")], m_BucketField],
        @[[self makeLabel:NSLocalizedString(@"Credentials:", "")], m_CredentialsPopup],
        @[[self makeLabel:NSLocalizedString(@"Access Key ID:", "")], m_AccessKeyField],
        @[[self makeLabel:NSLocalizedString(@"Secret Access Key:", "")], m_SecretKeyField],
        @[NSGridCell.emptyContentView, m_PathStyleCheckbox],
        @[NSGridCell.emptyContentView, m_HintLabel],
    ]];
    grid.translatesAutoresizingMaskIntoConstraints = false;
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;

    NSButton *const cancel = [NSButton buttonWithTitle:NSLocalizedString(@"Cancel", "")
                                                target:self
                                                action:@selector(onCancel:)];
    cancel.keyEquivalent = @"\e";
    m_ConnectButton = [NSButton buttonWithTitle:NSLocalizedString(@"Connect", "")
                                         target:self
                                         action:@selector(onConnect:)];
    m_ConnectButton.keyEquivalent = @"\r";

    NSStackView *const buttons = [NSStackView stackViewWithViews:@[cancel, m_ConnectButton]];
    buttons.translatesAutoresizingMaskIntoConstraints = false;
    buttons.spacing = 12;

    NSView *const content = self.window.contentView;
    [content addSubview:grid];
    [content addSubview:buttons];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [grid.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [buttons.topAnchor constraintEqualToAnchor:grid.bottomAnchor constant:20],
        [buttons.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
    ]];
    self.window.initialFirstResponder = m_TitleField;
}

//==================================================================================================
// State
//==================================================================================================

- (void)beginSheetForWindow:(NSWindow *)_wnd completionHandler:(void (^)(NSModalResponse returnCode))_handler
{
    [self loadStateIntoInterface];
    [super beginSheetForWindow:_wnd completionHandler:_handler];
}

- (void)loadStateIntoInterface
{
    if( self.setupMode )
        m_ConnectButton.title = NSLocalizedString(@"Save", "");

    if( m_Original ) {
        const auto &c = m_Original->Get<NetworkConnectionsManager::S3>();
        m_TitleField.stringValue = S3SafeNSString(c.title);
        [m_ServicePopup selectItemWithTag:static_cast<NSInteger>(c.endpoint.empty() ? S3Service::AWS : S3Service::Custom)];
        m_EndpointField.stringValue = S3SafeNSString(c.endpoint);
        m_RegionField.stringValue = S3SafeNSString(c.region);
        m_BucketField.stringValue = S3SafeNSString(c.bucket);
        m_AccessKeyField.stringValue = S3SafeNSString(c.access_key);
        m_PathStyleCheckbox.state = c.path_style ? NSControlStateValueOn : NSControlStateValueOff;
        [m_CredentialsPopup selectItemWithTag:g_ManualKeysTag];
        for( size_t i = 0; i < m_Profiles.size(); ++i )
            if( !c.aws_profile.empty() && m_Profiles[i].name == c.aws_profile )
                [m_CredentialsPopup selectItemWithTag:static_cast<NSInteger>(i)];
    }
    if( m_Password )
        m_SecretKeyField.stringValue = S3SafeNSString(*m_Password);

    [self updateControlsState];
}

- (const nc::vfs::s3::AwsProfile *)selectedProfile
{
    const NSInteger tag = m_CredentialsPopup.selectedTag;
    if( tag < 0 || static_cast<size_t>(tag) >= m_Profiles.size() )
        return nullptr;
    return &m_Profiles[static_cast<size_t>(tag)];
}

- (bool)isCustomService
{
    return m_ServicePopup.selectedTag == static_cast<NSInteger>(S3Service::Custom);
}

- (void)updateControlsState
{
    const auto profile = [self selectedProfile];
    const bool custom = [self isCustomService];

    m_EndpointField.enabled = custom;
    m_PathStyleCheckbox.enabled = custom;
    m_AccessKeyField.enabled = profile == nullptr;
    m_SecretKeyField.enabled = profile == nullptr;

    if( profile ) {
        m_AccessKeyField.placeholderString = S3SafeNSString(profile->access_key);
        m_SecretKeyField.placeholderString = NSLocalizedString(@"read from ~/.aws on every connect", "");
        m_RegionField.placeholderString = profile->region.empty() ? @"us-east-1" : S3SafeNSString(profile->region);
        if( !profile->endpoint.empty() )
            m_EndpointField.placeholderString = S3SafeNSString(profile->endpoint);
    }
    else {
        m_AccessKeyField.placeholderString = @"AKIA...";
        m_SecretKeyField.placeholderString = NSLocalizedString(@"stored in the Keychain", "");
        m_RegionField.placeholderString = @"us-east-1";
        m_EndpointField.placeholderString = @"https://minio.example.com:9000";
    }

    m_HintLabel.stringValue =
        custom ? NSLocalizedString(@"MinIO and most self-hosted services need path-style addressing. "
                                   @"Cloudflare R2 uses the region “auto”.",
                                   "")
               : NSLocalizedString(@"Buckets in other regions are detected automatically.", "");

    m_ConnectButton.enabled = [self isValid];
}

- (bool)isValid
{
    if( [self isCustomService] ) {
        const bool profile_has_endpoint = [self selectedProfile] && ![self selectedProfile]->endpoint.empty();
        const std::string endpoint = S3TrimmedStdString(m_EndpointField);
        if( endpoint.empty() ? !profile_has_endpoint : !nc::vfs::s3::ParseEndpoint(endpoint) )
            return false;
    }
    if( [self selectedProfile] )
        return true;
    return !S3TrimmedStdString(m_AccessKeyField).empty() && m_SecretKeyField.stringValue.length > 0;
}

//==================================================================================================
// Actions
//==================================================================================================

- (void)onServiceChanged:(id) [[maybe_unused]] _sender
{
    if( [self isCustomService] && m_PathStyleCheckbox.state == NSControlStateValueOff &&
        m_EndpointField.stringValue.length == 0 )
        m_PathStyleCheckbox.state = NSControlStateValueOn; // the sensible default for self-hosted services
    [self updateControlsState];
}

- (void)onCredentialsChanged:(id) [[maybe_unused]] _sender
{
    [self updateControlsState];
}

- (void)controlTextDidChange:(NSNotification *) [[maybe_unused]] _notification
{
    [self updateControlsState];
}

- (void)onConnect:(id) [[maybe_unused]] _sender
{
    if( ![self isValid] )
        return;

    m_Result = {};
    m_Result.uuid = m_Original ? m_Original->Uuid() : NetworkConnectionsManager::MakeUUID();
    m_Result.title = S3TrimmedStdString(m_TitleField);
    m_Result.endpoint = [self isCustomService] ? S3TrimmedStdString(m_EndpointField) : std::string{};
    m_Result.region = S3TrimmedStdString(m_RegionField);
    m_Result.bucket = S3TrimmedStdString(m_BucketField);
    m_Result.path_style = [self isCustomService] && m_PathStyleCheckbox.state == NSControlStateValueOn;

    if( const auto profile = [self selectedProfile] ) {
        m_Result.aws_profile = profile->name;
        m_Result.access_key = profile->access_key; // informational only, credentials are re-read on connect
        m_Password = std::string{};
    }
    else {
        m_Result.access_key = S3TrimmedStdString(m_AccessKeyField);
        const char *const secret = m_SecretKeyField.stringValue.UTF8String;
        m_Password = std::string(secret ? secret : "");
    }

    [self endSheet:NSModalResponseOK];
}

- (void)onCancel:(id) [[maybe_unused]] _sender
{
    [self endSheet:NSModalResponseCancel];
}

//==================================================================================================
// ConnectionSheetProtocol
//==================================================================================================

- (void)setConnection:(NetworkConnectionsManager::Connection)_connection
{
    m_Original = _connection;
}

- (NetworkConnectionsManager::Connection)connection
{
    return NetworkConnectionsManager::Connection(m_Result);
}

- (void)setPassword:(std::string)_password
{
    m_Password = _password;
}

- (std::string)password
{
    return m_Password ? *m_Password : "";
}

@end
