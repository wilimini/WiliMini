package bili

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
	"slices"
	"sort"
	"strconv"
	"strings"
)

// ErrNoStream means Bilibili offered no H.264 or HEVC rendition we can transcode.
var ErrNoStream = errors.New("no usable H.264 or HEVC stream")

type dashItem struct {
	ID         int      `json:"id"`
	BaseURL    string   `json:"base_url"`
	BaseURLAlt string   `json:"baseUrl"`
	Backup     []string `json:"backup_url"`
	BackupAlt  []string `json:"backupUrl"`
	Bandwidth  int      `json:"bandwidth"`
	CodecID    int      `json:"codecid"`
	Width      int      `json:"width"`
	Height     int      `json:"height"`
	FrameRate  string   `json:"frame_rate"`
	FrameRate2 string   `json:"frameRate"`
}

// fps is the rendition's frame rate, or 0 when Bilibili did not say.
func (d dashItem) fps() float64 {
	for _, s := range []string{d.FrameRate, d.FrameRate2} {
		if v, err := strconv.ParseFloat(strings.TrimSpace(s), 64); err == nil && v > 0 {
			return v
		}
	}
	return 0
}

func (d dashItem) urls() []string {
	var out []string
	if d.BaseURL != "" {
		out = append(out, d.BaseURL)
	} else if d.BaseURLAlt != "" {
		out = append(out, d.BaseURLAlt)
	}
	out = append(out, d.Backup...)
	out = append(out, d.BackupAlt...)
	return out
}

type playData struct {
	Dash *struct {
		Video []dashItem `json:"video"`
		Audio []dashItem `json:"audio"`
	} `json:"dash"`
	Durl []struct {
		URL    string   `json:"url"`
		Backup []string `json:"backup_url"`
	} `json:"durl"`
	// Present in the PGC (bangumi/film) envelope.
	VideoInfo *struct {
		Dash *struct {
			Video []dashItem `json:"video"`
			Audio []dashItem `json:"audio"`
		} `json:"dash"`
		Durl []struct {
			URL    string   `json:"url"`
			Backup []string `json:"backup_url"`
		} `json:"durl"`
	} `json:"video_info"`
}

// Media is what ffmpeg needs: one muxed URL, or separate video and audio URLs.
// Each slice lists the primary URL first and CDN fallbacks after it.
type Media struct {
	Video  []string
	Audio  []string
	Muxed  []string
	QN     int
	Width  int
	Height int
	FPS    float64 // source frame rate; 0 when unknown
	// Qualities lists every tier this video can be played at (ascending, one entry per tier),
	// so the PSP can offer exactly those. Empty for a muxed fallback stream, which has one quality.
	Qualities []int
}

// PlayRequest identifies what to play.
type PlayRequest struct {
	BVID string
	CID  int64
	QN   int  // preferred quality id; 32 = 480P
	PGC  bool // bangumi / film episode instead of a normal upload
}

// Bilibili quality ids. The PSP screen is 480x272, but the server decodes the source and scales it
// down, so any tier is usable; higher ones only cost the PC more work. The 60 fps tiers and 4K
// need a signed-in account (some need VIP); a tier the account cannot get falls back to the best
// one below it.
const (
	QN360      = 16
	QN480      = 32
	QN720      = 64
	QN720P60   = 74
	QN1080     = 80
	QN1080Plus = 112
	QN1080P60  = 116
	QN4K       = 120
)

// Codec ids in a DASH list. AVC is the cheapest to decode and is always preferred at a given
// quality; HEVC is accepted because the highest tiers are often offered in nothing else, and
// ffmpeg on the PC decodes it. AV1 is skipped: it needs an optional ffmpeg decoder.
const (
	codecAVC  = 7
	codecHEVC = 12
)

const (
	fnvalDash = 16  // DASH container
	fnval4K   = 128 // allow 4K renditions
)

// PlayURL asks Bilibili for stream URLs and picks an AVC rendition.
func (c *Client) PlayURL(ctx context.Context, req PlayRequest) (*Media, error) {
	if req.QN <= 0 {
		req.QN = QN480
	}
	params := url.Values{}
	params.Set("cid", strconv.FormatInt(req.CID, 10))
	params.Set("qn", strconv.Itoa(req.QN))
	fnval, fourk := fnvalDash, "0"
	if req.QN >= QN4K {
		fnval |= fnval4K
		fourk = "1"
	}
	params.Set("fnval", strconv.Itoa(fnval))
	params.Set("fnver", "0")
	params.Set("fourk", fourk)
	var data json.RawMessage
	var err error
	if req.PGC {
		params.Set("from_client", "PC_APP")
		params.Set("drm_tech_type", "3")
		data, err = c.GetJSON(ctx, c.APIBase+"/pgc/player/web/v2/playurl?"+params.Encode())
	} else {
		params.Set("bvid", req.BVID)
		params.Set("from_client", "BROWSER")
		params.Set("web_location", "1315873")
		params.Set("gaia_source", "view-card")
		params.Set("is_main_page", "false")
		params.Set("need_fragment", "false")
		params.Set("isGaiaAvoided", "true")
		params.Set("voice_balance", "1")
		data, err = c.SignedGet(ctx, c.APIBase+"/x/player/wbi/playurl", params)
	}
	if err != nil {
		return nil, err
	}
	return SelectMedia(data, req.QN)
}

// SelectMedia chooses streams from a playurl "data" object.
func SelectMedia(data json.RawMessage, wantQN int) (*Media, error) {
	var pd playData
	if err := json.Unmarshal(data, &pd); err != nil {
		return nil, err
	}
	dash, durl := pd.Dash, pd.Durl
	if pd.VideoInfo != nil {
		dash, durl = pd.VideoInfo.Dash, pd.VideoInfo.Durl
	}
	if dash != nil && len(dash.Video) > 0 {
		video := pickVideo(dash.Video, wantQN)
		if video != nil {
			m := &Media{Video: video.urls(), QN: video.ID, Width: video.Width, Height: video.Height, FPS: video.fps(),
				Qualities: playableTiers(dash.Video)}
			if audio := pickAudio(dash.Audio); audio != nil {
				m.Audio = audio.urls()
			}
			if len(m.Video) > 0 && len(m.Audio) > 0 {
				return m, nil
			}
		}
	}
	if len(durl) > 0 && durl[0].URL != "" {
		return &Media{Muxed: append([]string{durl[0].URL}, durl[0].Backup...), QN: wantQN}, nil
	}
	return nil, ErrNoStream
}

// pickVideo takes the largest quality not above wantQN, otherwise the smallest above it. Within
// one quality it prefers AVC over HEVC, then the lower bandwidth (cheaper to decode).
func pickVideo(items []dashItem, wantQN int) *dashItem {
	var usable []dashItem
	for _, it := range items {
		if (it.CodecID == codecAVC || it.CodecID == codecHEVC) && len(it.urls()) > 0 {
			usable = append(usable, it)
		}
	}
	if len(usable) == 0 {
		return nil
	}
	sort.SliceStable(usable, func(i, j int) bool {
		a, b := usable[i], usable[j]
		aOK, bOK := a.ID <= wantQN, b.ID <= wantQN
		switch {
		case aOK != bOK:
			return aOK
		case a.ID != b.ID && aOK:
			return a.ID > b.ID
		case a.ID != b.ID:
			return a.ID < b.ID
		case a.CodecID != b.CodecID:
			return a.CodecID == codecAVC
		}
		return a.Bandwidth < b.Bandwidth
	})
	return &usable[0]
}

// playableTiers is the distinct quality ids of the renditions pickVideo could choose, ascending.
func playableTiers(items []dashItem) []int {
	var tiers []int
	for _, it := range items {
		if (it.CodecID != codecAVC && it.CodecID != codecHEVC) || len(it.urls()) == 0 {
			continue
		}
		if !slices.Contains(tiers, it.ID) {
			tiers = append(tiers, it.ID)
		}
	}
	sort.Ints(tiers)
	return tiers
}

func pickAudio(items []dashItem) *dashItem {
	for _, id := range []int{30280, 30232, 30216} {
		for i := range items {
			if items[i].ID == id && len(items[i].urls()) > 0 {
				return &items[i]
			}
		}
	}
	for i := range items {
		if len(items[i].urls()) > 0 {
			return &items[i]
		}
	}
	return nil
}
