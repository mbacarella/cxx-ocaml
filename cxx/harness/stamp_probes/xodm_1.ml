module Order = struct module type Total = sig type t val compare: t -> t -> int end end
module F (X : Order.Total) = struct let f = X.compare end
