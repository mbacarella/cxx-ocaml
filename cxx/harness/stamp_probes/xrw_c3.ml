module type P = sig type t val compare : t -> t -> int end
module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : P with type t = M.t = struct type t = M.t let compare = compare end
