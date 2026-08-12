/*
 * libdatachannel example web server (cloud version — listens on 0.0.0.0)
 */
const http = require('http');
const webSocket = require('websocket');

const clients = {};

const httpServer = http.createServer((req, res) => {
  const respond = (code, data, contentType = 'text/plain') => {
    res.writeHead(code, {
      'Content-Type': contentType,
      'Access-Control-Allow-Origin': '*',
    });
    res.end(data);
  };
  respond(404, 'Not Found');
});

const wsServer = new webSocket.server({ httpServer });
wsServer.on('request', (req) => {
  console.log(`WS  ${req.resource}`);

  const { path } = req.resourceURL;
  const splitted = path.split('/');
  splitted.shift();
  const id = splitted[0];

  const conn = req.accept(null, req.origin);
  conn.on('message', (data) => {
    if (data.type === 'utf8') {
      console.log(`Client ${id} << ${data.utf8Data}`);

      const message = JSON.parse(data.utf8Data);
      const destId = message.id;
      const dest = clients[destId];
      if (dest) {
        message.id = id;
        const out = JSON.stringify(message);
        console.log(`Client ${destId} >> ${out}`);
        dest.send(out);
      } else {
        console.error(`Client ${destId} not found`);
      }
    }
  });
  conn.on('close', () => {
    delete clients[id];
    console.error(`Client ${id} disconnected`);
  });

  clients[id] = conn;
});

const port = process.env.PORT || '8000';
const host = process.env.HOST || '0.0.0.0';

httpServer.listen(port, host, () => {
  console.log(`Signaling server listening on ${host}:${port}`);
});
