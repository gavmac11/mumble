#!/usr/bin/env python3
"""Seed/check recovery behavior on an explicitly owned private Mumble guest.

Creates a permanent room, certificate-registered member, group/ACL and a
certificate-only ban. Checks actual authorized/denied entry and banned login.
This is a protocol acceptance drill, not an interactive native client test.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import ssl
import struct
import sys
import time

from relay_probe import blob, integer, read_varint


ROOM = 'Recovery Room'
MEMBER = 'recovery-member'
BANNED = 'recovery-banned'
GROUP = 'recovery-members'


def values(data):
    result = {}
    offset = 0
    while offset < len(data):
        tag, offset = read_varint(data, offset)
        field, wire = tag >> 3, tag & 7
        if not field:
            raise ValueError('Invalid protobuf field')
        if wire == 0:
            value, offset = read_varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                length, offset = read_varint(data, offset)
            else:
                length = 8 if wire == 1 else 4
            if length > len(data) - offset:
                raise ValueError('Truncated protobuf field')
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError('Unsupported protobuf wire type')
        result.setdefault(field, []).append(value)
    return result


def one(data, field, default=None):
    return data.get(field, [default])[-1]


def certificate_ban_identifier(peer):
    """Use the server's opaque UserState hash, as native ban administration does.

    This preserves old-server BanList compatibility without constructing a new
    weak certificate digest in the fixture. TLS trust remains CA validated and
    the recovery identity comparison uses SHA256 separately.
    """
    identifier = one(peer.users.get(peer.session, {}), 15)
    if (not isinstance(identifier, bytes) or len(identifier) != 40
            or any(value not in b'0123456789abcdef' for value in identifier)):
        raise AssertionError('Server did not supply a valid certificate ban identifier')
    return identifier.decode('ascii')


class Peer:
    def __init__(self, args, name, password='', certificate=None):
        self.args, self.name, self.password, self.certificate = args, name, password, certificate
        self.channels, self.users = {}, {}
        self.writer = None
        self.session = None

    async def send(self, kind, payload):
        self.writer.write(struct.pack('>HI', kind, len(payload)) + payload)
        await self.writer.drain()

    async def packet(self):
        kind, length = struct.unpack('>HI', await asyncio.wait_for(self.reader.readexactly(6), 10))
        if length > 1024 * 1024:
            raise ValueError('Oversized server message')
        raw = await asyncio.wait_for(self.reader.readexactly(length), 10)
        data = values(raw)
        if kind == 7 and one(data, 1) is not None:
            self.channels.setdefault(one(data, 1), {}).update(data)
        elif kind == 9 and one(data, 1) is not None:
            self.users.setdefault(one(data, 1), {}).update(data)
        return kind, data

    async def wait(self, kind, predicate=lambda data: True):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            received, data = await self.packet()
            if received == kind and predicate(data):
                return data
            if received == 4:
                raise RuntimeError('Server rejected fixture login')
            if received == 12 and kind != 12:
                raise RuntimeError('Server refused fixture administration request')
        raise TimeoutError('Fixture response deadline expired')

    async def connect(self):
        context = ssl.create_default_context(cafile=str(self.args.ca_file))
        if self.certificate:
            context.load_cert_chain(self.args.secrets / (self.certificate + '.crt'),
                                    self.args.secrets / (self.certificate + '.key'))
        self.reader, self.writer = await asyncio.wait_for(asyncio.open_connection(
            self.args.host, self.args.port, ssl=context, server_hostname=self.args.server_name), 10)
        await self.send(0, integer(1, 0x10700) + blob(2, b'recovery-fixture'))
        await self.send(2, blob(1, self.name.encode()) + integer(5, 1)
                        + (blob(2, self.password.encode()) if self.password else b''))
        self.session = one(await self.wait(5), 1)
        return self

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (OSError, ssl.SSLError):
                pass


async def run(args):
    join = (args.secrets / 'join-password').read_text().strip()
    admin_password = (args.secrets / 'admin-password').read_text().strip()
    peers = []
    async def connect(name, password='', certificate=None):
        peer = Peer(args, name, password, certificate)
        peers.append(peer)
        return await peer.connect()
    try:
        admin = await connect('SuperUser', admin_password)
        server_certificate = admin.writer.get_extra_info('ssl_object').getpeercert(binary_form=True)
        identity = hashlib.sha256(server_certificate).hexdigest()
        if args.mode == 'seed':
            if any(one(channel, 3) == ROOM.encode() for channel in admin.channels.values()):
                raise ValueError('Fixture room already exists; seed only the designated fresh guest')
            member = await connect(MEMBER, join, 'member')
            await admin.send(9, integer(1, member.session) + integer(4, 0))
            registration = await admin.wait(9, lambda state: one(state, 1) == member.session
                                            and one(state, 4, 0) > 0)
            user_id = one(registration, 4)
            await admin.send(7, integer(2, 0) + blob(3, ROOM.encode())
                             + blob(5, b'Saved state recovery drill'))
            channel_id = one(await admin.wait(7, lambda channel: one(channel, 3) == ROOM.encode()), 1)
            group = blob(1, GROUP.encode()) + integer(3, 1) + integer(4, 1) + integer(5, user_id)
            deny = blob(5, b'all') + integer(1, 1) + integer(2, 1) + integer(7, 0xC)
            allow = blob(5, GROUP.encode()) + integer(1, 1) + integer(2, 1) + integer(6, 0xE)
            await admin.send(13, integer(1, channel_id) + integer(2, 0)
                             + blob(3, group) + blob(4, deny) + blob(4, allow))
            banned = await connect(BANNED, join, 'banned')
            banned_hash = certificate_ban_identifier(banned)
            if args.legacy_ban:
                # The old server lacks the certificate-only UserRemove extension.
                # A documentation IP avoids banning every NAT/loopback client.
                address = bytes.fromhex('00000000000000000000ffffc000027b')
                entry = (blob(1, address) + integer(2, 128) + blob(3, BANNED.encode())
                         + blob(4, banned_hash.encode()) + blob(5, b'Private schema rollback fixture'))
                await admin.send(10, blob(1, entry))
                await admin.send(8, integer(1, banned.session)
                                 + blob(3, b'Fixture kick after saving certificate ban'))
            else:
                await admin.send(8, integer(1, banned.session) + blob(3, b'Recovery certificate ban')
                                 + integer(4, 1) + integer(5, 1) + integer(6, 0))
            await admin.send(10, integer(2, 1))
            bans = await admin.wait(10)
            if not any(one(values(entry), 4) == banned_hash.encode() for entry in bans.get(1, [])):
                raise AssertionError('Fixture certificate ban was not saved')
            await member.close()
            record = {'schema': 1, 'channel_id': channel_id, 'registered_user_id': user_id,
                      'banned_certificate_sha1': banned_hash, 'server_certificate_sha256': identity,
                      'room': ROOM, 'member': MEMBER, 'group': GROUP}
            args.record.write_text(json.dumps(record, indent=2) + '\n')
        else:
            record = json.loads(args.record.read_text())
            channel_id, user_id = record['channel_id'], record['registered_user_id']
            if identity != record['server_certificate_sha256']:
                raise AssertionError('Server TLS identity changed across recovery')
            if one(admin.channels.get(channel_id, {}), 3) != ROOM.encode():
                raise AssertionError('Permanent room was not recovered')
        # Exercise certificate registration after disconnect, without supplying
        # the join password (registered certificate accounts use their identity).
        member = await connect(MEMBER, certificate='member')
        if one(member.users.get(member.session, {}), 4) != user_id:
            raise AssertionError('Registered certificate account was not recovered')
        if one(member.users.get(member.session, {}), 5, 0) == channel_id:
            # The remembered channel can already be the destination after a
            # restart. Leave first so this check proves a new authorized move.
            await member.send(9, integer(1, member.session) + integer(5, 0))
            await member.wait(9, lambda state: one(state, 1) == member.session and one(state, 5) == 0)
        await member.send(9, integer(1, member.session) + integer(5, channel_id))
        await member.wait(9, lambda state: one(state, 1) == member.session and one(state, 5) == channel_id)
        outsider = await connect('recovery-outsider', join)
        await outsider.send(9, integer(1, outsider.session) + integer(5, channel_id))
        await outsider.wait(12)
        await admin.send(13, integer(1, channel_id) + integer(5, 1))
        policy = await admin.wait(13)
        groups = [values(group) for group in policy.get(3, [])]
        rules = [values(rule) for rule in policy.get(4, [])]
        if not any(one(group, 1) == GROUP.encode() and user_id in group.get(5, []) for group in groups):
            raise AssertionError('Group membership was not recovered')
        if not (any(one(rule, 5) == b'all' and one(rule, 7) == 0xC for rule in rules)
                and any(one(rule, 5) == GROUP.encode() and one(rule, 6) == 0xE for rule in rules)):
            raise AssertionError('Saved ACL rules were not recovered')
        await admin.send(10, integer(2, 1))
        bans = await admin.wait(10)
        if not any(one(values(entry), 4) == record['banned_certificate_sha1'].encode() for entry in bans.get(1, [])):
            raise AssertionError('Saved certificate ban was not recovered')
        denied = Peer(args, BANNED, join, 'banned')
        peers.append(denied)
        sync_before_close = False
        try:
            await denied.connect()
            if args.legacy_ban:
                sync_before_close = True
                # Qt5 may finish queued authentication after requesting disconnect.
                # Require closure within a single total deadline, even if data keeps arriving.
                deadline = time.monotonic() + 2
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError
                    await asyncio.wait_for(denied.packet(), remaining)
        except TimeoutError:
            raise AssertionError('Banned certificate connection remained open')
        except (RuntimeError, asyncio.IncompleteReadError, OSError):
            pass
        else:
            raise AssertionError('Banned certificate unexpectedly logged in')
        result = {'mode': args.mode, 'administrator_login': True, 'tls_identity_matches': True,
                'permanent_channel_matches': True, 'registered_certificate_login': True,
                'member_channel_entry_allowed': True, 'outsider_channel_entry_denied': True,
                'group_membership_matches': True, 'acl_rules_match': True,
                'certificate_ban_matches': True, 'banned_certificate_login_denied': True}
        if args.legacy_ban:
            del result['banned_certificate_login_denied']
            result.update(banned_certificate_connection_closed=True,
                          banned_server_sync_before_close=sync_before_close)
        return result
    finally:
        await asyncio.gather(*(peer.close() for peer in peers), return_exceptions=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('seed', 'check'), required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--server-name', required=True)
    parser.add_argument('--ca-file', type=Path, required=True)
    parser.add_argument('--secrets', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--legacy-ban', action='store_true',
                        help='Explicit old-server BanList/closed-socket check; modern login denial stays strict')
    args = parser.parse_args()
    try:
        print(json.dumps(asyncio.run(run(args))))
    except (ValueError, OSError, RuntimeError, TimeoutError, AssertionError, asyncio.IncompleteReadError) as error:
        print(f'Recovery fixture failed: {type(error).__name__}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
