module F (X : sig type t end) = struct type r = { v : X.t } let mk v = { v } end
open F(struct type t = float end)
let g = 1
