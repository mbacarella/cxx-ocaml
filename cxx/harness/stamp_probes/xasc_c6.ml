module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
include (Map.Make(M) : Map.S)
