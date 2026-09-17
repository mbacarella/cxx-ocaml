module Create(P: sig type t end) = struct
    module Priority = P
    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
        method tag: 'level -> string
    end
end
