module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end
module Create(P: Order.Total) = struct
    type u = P.t
end
module Basic = Create(struct type t = int  let compare a b = b - a end)
