import { serviceGroups, showcaseCopy } from '../data/service-videos.mjs';
import manifest from '../../public/assets/service-recordings/manifest.json';

const root = document.querySelector('.pcv-showcase');
if (root) {
  const locale = root.dataset.locale === 'en' ? 'en' : 'ko';
  const copy = showcaseCopy[locale];
  const find = (selector) => root.querySelector(selector);
  const tabs = [...root.querySelectorAll('[data-service]')];
  const stage = find('#service-stage');
  const video = find('[data-service-video]');
  const poster = find('[data-video-poster]');
  const play = find('[data-video-play]');
  const retry = find('[data-video-retry]');
  const status = find('[data-video-status]');
  const sceneList = find('[data-scene-list]');
  let group = serviceGroups[0];
  let clip = group.clips[0];
  let generation = 0;
  let phase = 'ready';
  const duration = (id) => {
    const seconds = Math.round(manifest.videos.find(item => item.id === id).duration);
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  };
  const setStatus = (value) => { status.textContent = value; };
  const clearVideo = () => {
    generation += 1;
    phase = 'ready';
    video.pause();
    if (video.hasAttribute('src')) {
      video.removeAttribute('src');
      video.load();
    }
    video.hidden = true;
    poster.hidden = false;
    play.hidden = false;
    retry.hidden = true;
    stage.removeAttribute('aria-busy');
  };
  const fail = () => {
    phase = 'error';
    video.pause();
    video.hidden = true;
    poster.hidden = false;
    play.hidden = true;
    retry.hidden = false;
    stage.removeAttribute('aria-busy');
    setStatus(copy.failed);
  };
  const selectClip = (next) => {
    clearVideo();
    clip = next;
    poster.src = clip.poster;
    poster.alt = clip[locale].title;
    const info = manifest.videos.find(item => item.id === clip.id);
    poster.width = info.width;
    poster.height = info.height;
    find('.pcv-video-frame').style.aspectRatio = `${info.width} / ${info.height}`;
    video.setAttribute('aria-label', clip[locale].title);
    play.setAttribute('aria-label', `${copy.play}: ${clip[locale].title}`);
    find('[data-video-duration]').textContent = duration(clip.id);
    find('[data-video-direct]').href = clip.src;
    const original = find('[data-video-original]');
    original.hidden = !clip.original;
    if (clip.original) original.href = clip.original;
    else original.removeAttribute('href');
    find('[data-scene-title]').textContent = clip[locale].title;
    find('[data-scene-summary]').textContent = clip[locale].summary;
    sceneList.querySelectorAll('[data-scene]').forEach(button => {
      button.setAttribute('aria-pressed', String(button.dataset.scene === clip.id));
    });
    setStatus(copy.ready);
  };
  const selectGroup = (next) => {
    group = next;
    tabs.forEach(tab => {
      const selected = tab.dataset.service === group.id;
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
    });
    stage.setAttribute('aria-labelledby', `service-tab-${group.id}`);
    find('[data-video-service]').textContent = group.label;
    find('[data-service-tag]').textContent = group[locale].tag;
    find('[data-service-title]').textContent = group[locale].title;
    find('[data-service-description]').textContent = group[locale].description;
    find('[data-service-note]').textContent = group[locale].note;
    find('#service-video-context').textContent = group[locale].context || copy.context;
    find('[data-service-guide]').href = group.guide || '/ko/infrastructure/networking/';
    find('[data-service-guide-label]').textContent = group[locale].guide || copy.guide;
    sceneList.replaceChildren(...group.clips.map((item, index) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'pcv-video-scene';
      button.dataset.scene = item.id;
      const number = document.createElement('span');
      number.className = 'pcv-video-scene-number';
      number.setAttribute('aria-hidden', 'true');
      number.textContent = `0${index + 1}`;
      const title = document.createElement('span');
      title.textContent = item[locale].title;
      const time = document.createElement('span');
      time.className = 'pcv-video-scene-time';
      time.textContent = duration(item.id);
      button.append(number, title, time);
      return button;
    }));
    selectClip(group.clips[0]);
  };
  const start = async () => {
    if (phase === 'loading' || phase === 'playing') return;
    const requestGeneration = ++generation;
    phase = 'loading';
    retry.hidden = true;
    poster.hidden = true;
    play.hidden = true;
    video.hidden = false;
    video.src = clip.src;
    stage.setAttribute('aria-busy', 'true');
    setStatus(copy.loading);
    video.focus();
    try {
      await video.play();
      if (requestGeneration !== generation) return;
      phase = 'playing';
      stage.removeAttribute('aria-busy');
      setStatus(copy.playing);
    } catch (error) {
      if (requestGeneration !== generation) return;
      fail();
      retry.focus();
    }
  };
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => selectGroup(serviceGroups[index]));
    tab.addEventListener('keydown', event => {
      let target;
      if (event.key === 'ArrowRight') target = (index + 1) % tabs.length;
      if (event.key === 'ArrowLeft') target = (index - 1 + tabs.length) % tabs.length;
      if (event.key === 'Home') target = 0;
      if (event.key === 'End') target = tabs.length - 1;
      if (target === undefined) return;
      event.preventDefault();
      tabs[target].focus();
      selectGroup(serviceGroups[target]);
    });
  });
  sceneList.addEventListener('click', event => {
    const button = event.target.closest('[data-scene]');
    if (!button) return;
    const next = group.clips.find(item => item.id === button.dataset.scene);
    if (next) selectClip(next);
  });
  play.addEventListener('click', start);
  retry.addEventListener('click', start);
  video.addEventListener('error', () => {
    if (video.hasAttribute('src') && (phase === 'loading' || phase === 'playing')) fail();
  });
  video.addEventListener('waiting', () => {
    if (phase === 'playing') setStatus(copy.loading);
  });
  video.addEventListener('playing', () => {
    if (phase === 'ready' || phase === 'error') return;
    phase = 'playing';
    stage.removeAttribute('aria-busy');
    setStatus(copy.playing);
  });
  video.addEventListener('pause', () => {
    if (phase === 'playing' && !video.ended) setStatus(copy.paused);
  });
  video.addEventListener('ended', () => {
    if (phase === 'playing') setStatus(copy.ended);
  });
  window.addEventListener('pagehide', () => { clearVideo(); setStatus(copy.ready); });
}
