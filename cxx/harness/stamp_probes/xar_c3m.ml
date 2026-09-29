module type S = sig type key type t type u = t end module SS = struct type t =
  string let compare = compare end module MS = Map.Make(SS) module HofM (M:
  Map.S) : S with type key = M.key = struct type key = M.key type t = (key, int)
  Hashtbl.t type u = t end module HS1 = HofM(MS)
