module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end

module type P = sig
    include Order.Total
end
module type R = sig
    module Priority: Order.Total
    type u = Priority.N.s
end
