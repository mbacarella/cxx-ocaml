module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end

module type R = sig
    module Priority: Order.Total
    type u = Priority.t
end
module type Q = Order.Total with type t = int
