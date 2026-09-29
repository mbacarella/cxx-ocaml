module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end
module type P = sig
    module Priority: Order.Total
    include Order.Total
end
