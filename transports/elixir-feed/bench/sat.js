/**
 * Load for the A/B bench (bench/ab.exs): bench/load.js's request mix with the think time removed.
 *
 *   MODE=sat   closed loop: VUS users, each sends its next request as soon as the last one answers.
 *              The most the server will do, as long as k6 itself is not the bottleneck.
 *   MODE=rate  open loop: RATE user iterations per second (about 2.2 requests each), whatever the
 *              server does. Both servers get the same request rate, so CPU per request compares
 *              like for like.
 *
 *   k6 run -e MODE=rate -e RATE=600 -e DURATION=20s -e TOKENS=seed/tokens.json -e SUMMARY=out.json bench/sat.js
 */
import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:3000';
const DURATION = __ENV.DURATION || '20s';
const VUS = Number(__ENV.VUS || 128);
const tokens = new SharedArray('tokens', () => JSON.parse(open(__ENV.TOKENS)));

const scenario = (__ENV.MODE || 'sat') === 'rate'
  ? { executor: 'constant-arrival-rate', rate: Number(__ENV.RATE || 600), timeUnit: '1s', duration: DURATION,
      preAllocatedVUs: VUS, maxVUs: VUS * 4 }
  : { executor: 'constant-vus', vus: VUS, duration: DURATION };

export const options = {
  scenarios: { bench: scenario },
  summaryTrendStats: ['avg', 'med', 'p(95)', 'p(99)', 'max'],
};

export default function () {
  const user = tokens[(__VU - 1) % tokens.length];
  const auth = { headers: { Authorization: `Bearer ${user.token}`, 'Content-Type': 'application/json' } };
  const feed = http.get(`${BASE_URL}/feed`);
  if (!check(feed, { 'feed 200': (r) => r.status === 200 })) return;
  const posts = feed.json('posts');
  const post = posts[Math.floor(Math.random() * posts.length)];
  check(http.get(`${BASE_URL}/posts/${post.id}`), { 'post 200': (r) => r.status === 200 });
  if (Math.random() < 0.15) {
    check(http.post(`${BASE_URL}/posts/${post.id}/like`, null, auth), { 'like': (r) => r.status === 200 || r.status === 201 });
  }
  if (Math.random() < 0.02) {
    const body = JSON.stringify({ body: `${user.username} says hi (VU ${__VU}, iter ${__ITER})` });
    check(http.post(`${BASE_URL}/posts`, body, auth), { 'create 201': (r) => r.status === 201 });
  }
}

export function handleSummary(data) {
  return { [__ENV.SUMMARY || '/dev/null']: JSON.stringify(data) };
}
