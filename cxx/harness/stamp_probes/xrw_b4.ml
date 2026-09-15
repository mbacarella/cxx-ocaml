module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : Map.S = Map.Make(M)
