module F (X : Set.OrderedType) = struct
  module rec M : sig type t = X.t val compare : t -> t -> int end =
  struct type t = X.t let compare = X.compare end
  and S : Set.OrderedType =
  struct type t = X.t let compare = X.compare end
end
