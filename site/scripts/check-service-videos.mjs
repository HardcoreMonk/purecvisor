import assert from 'node:assert/strict';
import {readFile, readdir, stat} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {serviceGroups} from '../src/data/service-videos.mjs';

const root = new URL('../dist/', import.meta.url);
const directory = new URL('assets/service-videos/', root);
const manifest = JSON.parse(await readFile(new URL('manifest.json', directory), 'utf8'));
const clips = serviceGroups.flatMap(group => group.clips);
assert.equal(new Set(clips.map(clip => clip.id)).size, 6);
assert.equal(serviceGroups.length, 3);
assert.equal(manifest.videos.length, clips.length);
assert.equal(new Set(manifest.videos.map(video => video.id)).size, clips.length);
const expected = ['manifest.json'];
let bytes = 0;
for (const clip of clips) {
  const info = manifest.videos.find(video => video.id === clip.id);
  assert(info && info.silent && info.duration > 0);
  assert.equal(clip.src, `/assets/service-videos/${clip.id}.mp4`);
  assert.equal(clip.poster, `/assets/service-videos/${clip.id}.webp`);
  for (const [extension, digest] of [['mp4', info.sha256], ['webp', info.posterSha256]]) {
    const filename = `${clip.id}.${extension}`;
    expected.push(filename);
    const data = await readFile(new URL(filename, directory));
    assert.equal(createHash('sha256').update(data).digest('hex'), digest, filename);
    bytes += data.length;
    if (extension === 'webp') {
      assert.equal(data.toString('ascii', 0, 4), 'RIFF');
      assert.equal(data.toString('ascii', 8, 12), 'WEBP');
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
assert(bytes < 10 * 1024 * 1024, 'showcase media exceeds budget');
assert.deepEqual((await readdir(directory)).sort(), expected.sort());
for (const path of ['index.html', 'ko/index.html', 'en/index.html']) {
  const html = await readFile(new URL(path, root), 'utf8');
  const players = [...html.matchAll(/<video\b[^>]*>/g)];
  assert.equal(players.length, 1);
  assert(!/\ssrc=|\sautoplay\b/.test(players[0][0]), 'initial video must not download or autoplay');
  assert(players[0][0].includes('preload="none"'));
  assert.equal((html.match(/role="tab"/g) || []).length, 3);
  assert(html.includes('id="service-scene-summary"') && html.includes('aria-live="polite"'));
  assert(html.indexOf('id="service-demos"') < html.indexOf('id="documentation"'));
  for (const clip of clips) assert(html.includes(`href="${clip.src}"`), 'noscript media fallback');
}
console.log(`service videos: 3 groups, 6 clips, ${bytes} bytes, hashes/faststart/lazy loading/fallback PASS`);
