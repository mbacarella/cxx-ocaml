module Order = struct module type Total = sig type t val compare: t -> t -> int end end
module F (X : sig module Priority: Order.Total end) = struct let f = X.Priority.compare end
