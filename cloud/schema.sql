-- Execute no SQL Editor de um projeto Supabase dedicado a este protótipo.
create table if not exists public.device_state (
  device_id text primary key,
  latest jsonb not null,
  last_seen timestamptz not null default now()
);

create table if not exists public.commands (
  id uuid primary key,
  device_id text not null,
  action text not null check (action in ('on','off','auto','schedules')),
  payload jsonb not null default '{}'::jsonb,
  status text not null check (status in ('pending','received','executed','rejected','expired','superseded')),
  idempotency_key text not null unique,
  created_by text not null,
  result jsonb,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  expires_at timestamptz not null
);

create index if not exists commands_device_status_created
  on public.commands (device_id, status, created_at desc);

alter table public.device_state enable row level security;
alter table public.commands enable row level security;
-- Sem políticas para anon/authenticated: todo acesso passa pelas Vercel Functions
-- usando service role em ambiente de servidor; nunca distribuir essa chave.
