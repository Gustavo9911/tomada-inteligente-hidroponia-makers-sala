# Painel remoto Vercel + Supabase

Esta pasta é uma aplicação estática servida pela Vercel com funções Node.js em `api/`. O frontend não recebe a chave de serviço nem o token do ESP32. Vercel valida a sessão do painel e as requisições do ESP32; Supabase fornece Postgres/PostgREST e a fila. O transporte do ESP32 é iniciado para fora por HTTPS, portanto não há porta a abrir no roteador.

## Componentes e dependências

- Necessário: projeto Supabase para Postgres e projeto Vercel para frontend + funções.
- Autenticação do painel: usuário/senha validados por uma função Vercel; a sessão é um cookie HttpOnly, Secure e SameSite assinado. A senha é armazenada na Vercel somente como hash PBKDF2.
- Funções Vercel: funções JavaScript sem dependência npm adicional; requerem suporte Node.js com `fetch` global.
- Armazenamento: Postgres gerenciado pelo Supabase, definido em `schema.sql`. Não há serviço de mensagens adicional; o ESP32 consulta a fila por HTTPS.
- Opcional: domínio próprio, monitoramento de erros e backup automatizado. Podem gerar cobrança.

Os limites e preços dos níveis gratuitos mudam. Confira os planos atuais antes de habilitar telemetria frequente; cada ESP envia uma atualização de estado a cada 15 s e consulta comandos a cada 4 s, além dos pedidos do painel a cada 5 s. Desative ou aumente esses intervalos se o volume/custo ficar acima do necessário.

## Preparar Supabase

1. O projeto `hidroponia-adib` já foi criado na região São Paulo (`lwtmvvnrbbmyuqmbtotz`). Sua URL é `https://lwtmvvnrbbmyuqmbtotz.supabase.co`.
2. O esquema `schema.sql` já foi aplicado. As tabelas `device_state` e `commands` foram verificadas com RLS ativo e sem políticas de leitura/escrita direta; o acesso de negócio passa pelas funções Vercel.
3. Use uma chave secreta do backend. Prefira a nova `sb_secret_...` na seção **Secret keys**; o código também aceita a chave legada `service_role` (`eyJ...`). Guarde-a somente na variável server-side correspondente da Vercel. Nunca a coloque no firmware, site, Git ou Monitor Serial.

## Preparar Vercel

1. O painel já está publicado em [adib-hidroponia-mika-projects1.vercel.app](https://adib-hidroponia-mika-projects1.vercel.app), com as funções Node em `api/`.
2. Cadastre estas variáveis no ambiente Production (e Preview se for usar prévias):

| Variável | Valor |
|---|---|
| `SUPABASE_URL` | URL HTTPS do projeto Supabase. |
| `SUPABASE_SECRET_KEY` ou `SUPABASE_SERVICE_ROLE_KEY` | Use a nova chave `sb_secret_...` preferencialmente; o deployment atual também aceita a chave legada `service_role`. Segredo exclusivamente server-side. |
| `REMOTE_USERNAME` | Usuário permitido para o painel. |
| `REMOTE_PASSWORD_SALT` | Salt aleatório em Base64 para a senha. |
| `REMOTE_PASSWORD_HASH` | Hash PBKDF2-SHA256 da senha em Base64, 210.000 iterações. |
| `REMOTE_SESSION_SECRET` | Segredo aleatório de pelo menos 32 bytes para assinar cookies da sessão. |
| `DEVICE_ID` | Identificador fixo, por exemplo `hidroponia-adib-01`. |
| `DEVICE_TOKEN` | Token aleatório forte compartilhado apenas com o firmware e a API. Gere com `openssl rand -hex 32`. |

3. As variáveis Production já estão cadastradas e o deployment atualizado está publicado. Após alterá-las no futuro, faça um novo deploy. `/api/session` valida o cookie assinado e `/api/config` revela somente o `DEVICE_ID`; nunca a chave Supabase, o hash de senha ou `DEVICE_TOKEN`.
4. Entre no painel usando o usuário e a senha provisionados. O dispositivo aparecerá offline até enviar a primeira telemetria.

## Configurar o ESP32 para nuvem

1. O sketch `../TomadaHidroponia.ino` carrega `HydroSecrets.h`, arquivo privado local que contém Wi-Fi, credenciais do painel local, domínio HTTPS, `DEVICE_ID`, token exclusivo da placa e CA raiz GTS Root R1 para o domínio Vercel. Mantenha esse arquivo junto do sketch e fora de repositórios/compartilhamentos.
2. Confirme que a variável Vercel `DEVICE_TOKEN` é exatamente o mesmo token do sketch. Se a girar, atualize a variável e regrave o firmware. Validação TLS permanece ativa; não troque por `setInsecure()`.
3. Preencha/substitua `WIFI_SSID` e `WIFI_PASSWORD` no sketch se as credenciais locais mudaram. Compile e grave novamente após concluir as variáveis da Vercel. Sem internet, proteção, agenda e controle local continuam ativos.
4. Observe o diagnóstico `cloud` em `/api/status` e o painel remoto. Não imprima tokens, senha Wi-Fi nem CA no serial.

O token do dispositivo está no firmware gravado e pode ser extraído por alguém com acesso físico ao ESP32. Use token exclusivo para este dispositivo; para revogar, gere um novo `DEVICE_TOKEN` na Vercel e regrave o firmware. Para tokens de vários dispositivos, a arquitetura precisa migrar para credencial individual por dispositivo e tabela de provisionamento.

## Fluxo e rotas

| Endpoint | Origem e autenticação | Função |
|---|---|---|
| `GET /api/config` | Público, dados não secretos | Entrega somente o ID do dispositivo. |
| `POST /api/login` | Usuário/senha | Cria cookie HttpOnly assinado, com validade de 8 horas. |
| `GET /api/session` | Cookie de sessão válido | Confirma se o painel está autenticado. |
| `GET /api/status` | Cookie de sessão válido | Estado mais recente, horário recebido, histórico no estado e últimos comandos. |
| `POST /api/command` | Cookie de sessão válido | Registra `on`, `off`, `auto` ou `schedules`, com chave idempotente e validade de 90 s. Uma solicitação mais nova substitui comandos ainda pendentes. |
| `POST /api/logout` | Browser | Revoga o cookie de sessão atual. |
| `POST /api/device/telemetry` | ESP32 com Bearer `DEVICE_TOKEN` + `X-Device-ID` | Atualiza último estado/`last_seen`. O histórico embarcado é transportado com a telemetria. |
| `GET /api/device/commands` | ESP32 autenticado | Expira comandos velhos e entrega apenas o mais recente ainda válido. |
| `POST /api/device/ack` | ESP32 autenticado | Recebe confirmação `received`, depois `executed` ou `rejected`. |

Telemetria de 15 s determina o status online do painel (limite de frescor de 45 s). A fila expira solicitação em 90 s. O ID do comando é único; `lastCmd` em Preferences impede reexecução após reconexão/reenvio. A automação local e a proteção de nível baixo não dependem do estado da nuvem.

## BLE e compatibilidade

O painel hospedado em HTTPS tenta Web Bluetooth quando o navegador disponibiliza `navigator.bluetooth`. Se ausente, o controle indica a limitação; não finge conexão. Android Chrome/Edge são os candidatos usuais, mas o suporte depende do SO e da versão do navegador. O painel HTTP local em endereço privado pode não ser contexto seguro: use um app BLE com o protocolo documentado no README principal.

## Custos, limites e renovação

Supabase Postgres e Vercel possuem planos e limites próprios, sujeitos a mudança. Um projeto escolar pode começar nos níveis gratuitos se estiver dentro de limites, mas não há garantia de cota ou disponibilidade. Verifique preço, limites de requisição, armazenamento, retenção, backups, política de dados da escola e cobrança por excedente antes de publicar. A frequência dos dois polls do dispositivo é o principal consumo recorrente deste desenho.

Para mudar a senha, gere outro salt/hash PBKDF2 e atualize `REMOTE_PASSWORD_SALT` e `REMOTE_PASSWORD_HASH` na Vercel. Para renovar sessões, altere `REMOTE_SESSION_SECRET`. Para renovar o token do dispositivo, atualize `DEVICE_TOKEN` na Vercel e o arquivo privado `HydroSecrets.h`, depois regrave o ESP32. Chaves Vercel/Supabase não devem ser copiadas para `app.js`, `index.html`, repositórios públicos, prints ou Monitor Serial.

## Teste integrado

O backend em produção já foi verificado: página HTTP 200, configuração pública retorna somente o ID do dispositivo, `/api/session` sem sessão retorna 401 e login autenticado seguido de `/api/status` respondeu 200. O firmware físico ainda precisa ser gravado para testar telemetria e comandos reais. Depois de gravá-lo:

1. Entre com o usuário/senha provisionados; confira que `/api/status` sem cookie recebe 401.
2. Sem ESP32 ativo, envie comando e confirme que permanece pendente, depois expira; a tela não deve mostrar execução.
3. Ligue o ESP32 com CA e token válidos e aguarde a telemetria; verifique hora `last_seen`, sensores/estado sem valores inventados e Online.
4. Envie `off`; confirme as etapas pendente/recebido/executado e a mudança do estado reportado pelo firmware.
5. Envie `on` com boia indicando baixo; o firmware deve rejeitar e a nuvem apresentar o erro/estado confirmado sem contornar a proteção.
6. Desconecte a internet e valide que agenda e controle local continuam; o painel remoto deve marcar dados antigos/offline.

Os testes físicos de sensor, relé, BLE, telemetria recebida da placa e operação sem internet ainda não foram executados, pois a ESP32 não está conectada nesta etapa.
