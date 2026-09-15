module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
module Outer = struct
  module S : Map.S = Map.Make(M)
end
