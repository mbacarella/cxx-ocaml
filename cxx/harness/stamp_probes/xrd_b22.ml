module A = struct module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end end
module type P = sig
    module Priority: A.Order.Total
    type u = Priority.t
end
