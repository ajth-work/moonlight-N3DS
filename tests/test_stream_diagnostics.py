import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class StreamDiagnosticsContractTest(unittest.TestCase):
    def test_session_snapshot_has_correlation_and_elapsed_fields(self):
        diagnostics = (ROOT / "src" / "diagnostics.c").read_text()
        upload = (ROOT / "src" / "n3ds_main.cpp").read_text()
        self.assertIn("corr=%s session_ms=%llu", diagnostics)
        self.assertIn('"SESSION_END", "scope=%s id=%s outcome=%s end=%llu mono_ms=%llu duration_ms=%llu"', diagnostics)
        self.assertIn('\\"session_id\\":', upload)
        self.assertIn('\\"duration_ms\\":', upload)

    def test_shutdown_stages_record_bounded_outcomes(self):
        common = ROOT / "third_party" / "moonlight-common-c" / "src"
        connection = (common / "Connection.c").read_text()
        audio = (common / "AudioStream.c").read_text()
        video = (common / "VideoStream.c").read_text()
        self.assertIn('diag_stream_stop_request("connection_loop_ended", GS_OK)', (ROOT / "src" / "n3ds_main.cpp").read_text())
        for stage in ("input", "input_cleanup"):
            self.assertIn(f'diag_stream_stage_begin("{stage}")', connection)
            self.assertIn(f'diag_stream_stage_end("{stage}", "complete", false)', connection)
        for stage in ("audio_renderer_stop", "audio_receive_stop_and_join", "audio_renderer_cleanup"):
            self.assertIn(f'diag_stream_stage_begin("{stage}")', audio)
            self.assertIn(f'diag_stream_stage_end("{stage}", "complete", false)', audio)
        for stage in ("video_network_receive_interrupt", "video_network_receive_join", "video_decoder_interrupt", "video_decoder_join", "video_renderer_cleanup"):
            self.assertIn(f'diag_stream_stage_begin("{stage}")', video)
            self.assertIn(f'diag_stream_stage_end("{stage}", "complete", false)', video)
        self.assertIn("timeout=%s", (ROOT / "src" / "diagnostics.c").read_text())

    def test_health_and_upload_acknowledgement_are_explicit(self):
        diagnostics = (ROOT / "src" / "diagnostics.c").read_text()
        main = (ROOT / "src" / "n3ds_main.cpp").read_text()
        for channel in ('"input"', '"audio"', '"video"', '"decode"', '"present"'):
            self.assertIn(channel, diagnostics)
        self.assertIn('"STREAM_HEALTH"', diagnostics)
        self.assertIn('state=attempted', main)
        self.assertIn('state=%s acknowledged=%d http_status=%ld result=%d', main)
        self.assertIn('awaiting_http_response', main)


if __name__ == "__main__":
    unittest.main()
