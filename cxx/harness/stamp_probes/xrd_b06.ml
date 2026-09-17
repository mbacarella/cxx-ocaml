module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module type Profile = sig
    module Priority: Order.Total
    class virtual c : object val mutable limit_: Priority.t end
end
