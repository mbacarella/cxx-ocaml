module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end
module type P = sig
    module Priority: Order.Total
end
module type Q = sig
    include P
    type u = Priority.t
end
