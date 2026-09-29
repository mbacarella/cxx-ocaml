module Order = struct module type Total = sig type t val compare : t -> t -> int end end
module A = struct
  module F (X : Order.Total) = struct let f x = X.compare x x end
end
