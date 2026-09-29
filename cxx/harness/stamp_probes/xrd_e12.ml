module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end
module Create(P: Order.Total) = struct
    module Priority = P

    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
        method tag: 'level -> string
    end

    class ['level] event prioritizer level message =
        let prioritizer = (prioritizer :> 'level prioritizer) in
        object
            method prioritizer = prioritizer
            method level: 'level = level
            method message: string = message
        end
end
