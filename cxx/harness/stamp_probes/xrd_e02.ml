module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end

module type Profile = sig
    module Priority: Order.Total

    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
        method tag: 'level -> string
    end

    class ['level] event:
        'level #prioritizer -> 'level -> string ->
        object
            method prioritizer: 'level prioritizer
            method level: 'level
            method message: string
        end

    class type ['event] archiver = object
        constraint 'event = 'level #event
        method emit: 'event -> unit
    end

    class virtual ['archiver] agent:
        'level #prioritizer -> 'level -> 'archiver list ->
        object
            constraint 'event = 'level #event
            constraint 'archiver = 'event #archiver
            val mutable archivers_: 'archiver list
            val mutable limit_: Priority.t
            method virtual private event: 'level -> string -> 'event
            method setlimit: 'level -> unit
            method enabled: 'level -> bool
            method private put: 'a 'b. 'level -> ('event -> 'b) -> ('a, unit,
              string, string, string, 'b) format6 -> 'a
        end
end
