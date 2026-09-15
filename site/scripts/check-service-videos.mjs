import assert from 'node:assert/strict';
import {readFile, readdir, stat} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {serviceGroups} from '../src/data/service-videos.mjs';

const root = new URL('../dist/', import.meta.url);
const directory = new URL('assets/service-recordings/', root);
const manifest = JSON.parse(await readFile(new URL('manifest.json', directory), 'utf8'));
const clips = serviceGroups.flatMap(group => group.clips);
assert.equal(new Set(clips.map(clip => clip.id)).size, 16);
assert.deepEqual(serviceGroups.map(group => [group.id, group.clips.length]), [['vpc', 7], ['ovn', 4], ['vxlan', 4], ['gpu', 1]]);
assert.equal(manifest.videos.length, clips.length);
assert.equal(new Set(manifest.videos.map(video => video.id)).size, clips.length);
const expected = ['manifest.json'];
let bytes = 0;
for (const clip of clips) {
  const info = manifest.videos.find(video => video.id === clip.id);
  assert(info && info.silent && info.duration > 0);
  const gpu = clip.id === 'gpu-passthrough';
  assert.equal(info.width, gpu ? 1920 : 1440);
  assert.equal(info.height, gpu ? 1080 : 900);
  assert.equal(info.editing, gpu ? 'korean-captions-and-audit-summary' : 'none');
  assert.equal(info.sha256, info.sourceMp4Sha256);
  assert(info.sourceMp4.endsWith('.mp4'));
  if (gpu) {
    assert.equal(info.sha256, 'e02e2d32d9a74a04d5eb1f490ab25de15f986ac63bc72b5394af6d3b46b396db');
    assert.equal(info.duration, 124);
    assert.equal(info.bytes, 36917780);
    assert.equal(clip.original, undefined);
    assert.equal(info.sourceOriginal, undefined);
    assert.equal(serviceGroups.find(group => group.id === 'gpu').guide, '/ko/workloads/virtual-machines/');
    for (const locale of ['ko', 'en']) {
      const copy = serviceGroups.find(group => group.id === 'gpu')[locale];
      assert(copy.context && copy.guide && copy.note);
    }
  } else {
    assert(/^[a-f0-9]{64}$/.test(info.originalSha256));
    assert(info.sourceOriginal.endsWith('.webm'));
    assert.equal(clip.original, `/assets/service-recordings/${clip.id}.webm`);
  }
  assert.equal(clip.src, `/assets/service-recordings/${clip.id}.mp4`);
  assert.equal(clip.poster, `/assets/service-recordings/${clip.id}.webp`);
  const assets = [['mp4', info.sha256], ['webp', info.posterSha256]];
  if (!gpu) assets.push(['webm', info.originalSha256]);
  for (const [extension, digest] of assets) {
    const filename = `${clip.id}.${extension}`;
    expected.push(filename);
    const data = await readFile(new URL(filename, directory));
    assert(data.length < 50 * 1024 * 1024, `${filename} exceeds individual budget`);
    assert.equal(createHash('sha256').update(data).digest('hex'), digest, filename);
    bytes += data.length;
    if (extension === 'webp') {
      assert.equal(data.toString('ascii', 0, 4), 'RIFF');
      assert.equal(data.toString('ascii', 8, 12), 'WEBP');
    } else if (extension === 'webm') {
      assert.equal(data.readUInt32BE(0), 0x1a45dfa3);
      assert.equal(data.length, info.originalBytes);
    } else {
      assert.equal((await stat(new URL(filename, directory))).size, info.bytes);
      let offset = 0;
      const atoms = [];
      while (offset + 8 <= data.length) {
        const size = data.readUInt32BE(offset);
        assert(size >= 8 && offset + size <= data.length);
        atoms.push(data.toString('ascii', offset + 4, offset + 8));
        offset += size;
      }
      assert(atoms.indexOf('moov') >= 0 && atoms.indexOf('moov') < atoms.indexOf('mdat'), `${filename} faststart`);
    }
  }
}
assert(bytes < 250 * 1024 * 1024, 'showcase media exceeds budget');
assert.deepEqual((await readdir(directory)).sort(), expected.sort());
for (const path of ['index.html', 'ko/index.html', 'en/index.html']) {
  const html = await readFile(new URL(path, root), 'utf8');
  const players = [...html.matchAll(/<video\b[^>]*>/g)];
  assert.equal(players.length, 1);
  assert(!/\ssrc=|\sautoplay\b/.test(players[0][0]), 'initial video must not download or autoplay');
  assert(players[0][0].includes('preload="none"'));
  assert.equal((html.match(/role="tab"/g) || []).length, 4);
  assert(html.includes('id="service-tab-gpu"') && html.includes('GPU Passthrough'));
  assert(!html.includes('gpu-passthrough.webm') && !html.includes('href="undefined"'));
  assert(html.includes('id="service-scene-summary"') && html.includes('aria-live="polite"'));
  assert(html.includes('data-video-original') && html.includes('data-scene-title'));
  assert(!html.includes('/assets/service-videos/'), 'edited media must not remain linked');
  assert(html.indexOf('id="service-demos"') < html.indexOf('id="documentation"'));
  for (const clip of clips) {
    assert(html.includes(`href="${clip.src}"`), 'noscript media fallback');
    if (clip.original) assert(html.includes(`href="${clip.original}"`), 'noscript original fallback');
  }
}
console.log(`service videos: 4 groups, ${clips.length} recordings, ${bytes} bytes, originals/GPU/hashes/faststart/lazy loading/fallback PASS`);
