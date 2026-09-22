module rec A : sig type t = { x : int; y : B.t option } end
  = struct type t = { x : int; y : B.t option } end
and B : sig type t = { z : A.t } val g : t -> int end
  = struct type t = { z : A.t } let g _ = 1 end
