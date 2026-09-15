module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
module type T = Map.S
module S : T = Map.Make(M)
