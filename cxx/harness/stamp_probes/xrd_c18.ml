module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end

module type P = sig
    module Q : sig module Priority: Order.Total end
end
module F (X : P) = struct end
