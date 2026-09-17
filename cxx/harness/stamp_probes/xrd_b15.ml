module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module type P = sig
    module Priority: Order.Total
    module Q: Order.Total
    type u = Priority.t
    type w = Q.t
end
