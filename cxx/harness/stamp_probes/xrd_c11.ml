module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end
module type P = sig
    module Priority: Order.Total
end

module F (X : P) = struct let f (a : X.Priority.t) = () let g (b :
  X.Priority.t) = () end
