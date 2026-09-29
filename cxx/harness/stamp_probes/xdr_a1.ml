module Q = struct type t = int let compare = compare end
module M : sig type u end =
  struct module S = Set.Make (Q) type u = S.t end
