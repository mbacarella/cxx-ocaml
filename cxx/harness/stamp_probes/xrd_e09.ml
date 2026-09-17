module Create(P: sig type t end) = struct
    module Priority = P
    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
    end
    class ['level] event prioritizer level message =
        let prioritizer = (prioritizer :> 'level prioritizer) in
        object
            method prioritizer = prioritizer
            method level: 'level = level
            method message: string = message
        end
end
