module F (X : sig type t val compare : t -> t -> int end) = struct
  module rec M : sig type t = X.t val compare : t -> t -> int end =
  struct type t = X.t let compare = X.compare end
  and S : Set.OrderedType with type t = M.t =
  struct type t = M.t let compare = compare end
end
