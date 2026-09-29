module Order = struct module type Total = sig type t val compare: t -> t -> int val z : t end end
module F (X : Order.Total) = struct let f a = X.compare a a let g = X.z end
