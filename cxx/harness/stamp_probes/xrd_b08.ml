module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module type Profile = sig
    module Priority: Order.Total
    class type c = object method code: Priority.t method tag: Priority.t end
end
