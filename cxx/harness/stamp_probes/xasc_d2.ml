module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
module S : MoreLabels.Set.S = MoreLabels.Set.Make(M)
