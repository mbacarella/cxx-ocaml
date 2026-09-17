module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end

module type P = sig
    module Priority: Order.Total
    module Q: Order.Total
    type u = Priority.N.s
    type w = Q.N.s
end
