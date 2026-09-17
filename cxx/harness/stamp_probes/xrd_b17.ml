module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module P (Priority: Order.Total) = struct
    type u = Priority.t
end
